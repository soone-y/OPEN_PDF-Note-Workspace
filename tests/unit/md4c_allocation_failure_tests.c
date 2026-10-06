/* Test-only allocator instrumentation; never linked into the application. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void* live[4096];
static size_t live_count, allocation_count, fail_at, callback_count, abort_at;
static int injected;

static void require(int condition, const char* message)
{
    if(!condition) {
        fprintf(stderr, "MD4C allocation regression: %s (allocation=%zu, fail=%zu)\n",
                message, allocation_count, fail_at);
        exit(1);
    }
}

static size_t find_pointer(void* pointer)
{
    size_t i;
    for(i = 0; i < live_count; i++)
        if(live[i] == pointer) return i;
    require(0, "free/realloc of non-owned pointer (double free or borrowed storage)");
    return 0;
}

static int should_fail(void)
{
    allocation_count++;
    if(allocation_count == fail_at) { injected = 1; return 1; }
    return 0;
}

static void* test_malloc(size_t size)
{
    void* pointer;
    if(should_fail()) return NULL;
    pointer = malloc(size);
    require(pointer != NULL, "unexpected system allocation failure");
    require(live_count < sizeof(live) / sizeof(live[0]), "allocation tracker full");
    live[live_count++] = pointer;
    return pointer;
}

static void* test_realloc(void* pointer, size_t size)
{
    size_t i;
    void* replacement;
    if(pointer == NULL) return test_malloc(size);
    i = find_pointer(pointer);
    require(size != 0, "unhandled realloc-to-zero contract");
    if(should_fail()) return NULL;
    replacement = realloc(pointer, size);
    require(replacement != NULL, "unexpected system realloc failure");
    live[i] = replacement;
    return replacement;
}

static void test_free(void* pointer)
{
    size_t i;
    if(pointer == NULL) return;
    i = find_pointer(pointer);
    free(pointer);
    live[i] = live[--live_count];
}

#define malloc test_malloc
#define realloc test_realloc
#define free test_free
#ifndef MD4C_TEST_SOURCE
#define MD4C_TEST_SOURCE "../../third_party/md4c/src/md4c.c"
#endif
#include MD4C_TEST_SOURCE
#undef malloc
#undef realloc
#undef free

static int callback(void) { return ++callback_count == abort_at ? 7 : 0; }
static int block(MD_BLOCKTYPE type, void* detail, void* userdata)
{ (void)type; (void)detail; (void)userdata; return callback(); }
static int span(MD_SPANTYPE type, void* detail, void* userdata)
{ (void)type; (void)detail; (void)userdata; return callback(); }
static int text(MD_TEXTTYPE type, const MD_CHAR* contents, MD_SIZE size, void* userdata)
{ (void)type; (void)contents; (void)size; (void)userdata; return callback(); }

static void reset(size_t failure, size_t abort_callback)
{
    require(live_count == 0, "previous operation leaked memory");
    allocation_count = callback_count = 0;
    fail_at = failure;
    abort_at = abort_callback;
    injected = 0;
}

int main(void)
{
    const MD_CHAR* inputs[] = {
        _T("[ref]: /url \"a &amp; b &quot; c\"\n\n[link][ref]\n"),
        _T("[a\\*b]: /u\n \"multi\n line &amp; title\"\n\n[x][a*b]\n"),
        _T("[a]: /a\n[b]: /b\n[c]: /c\n[d]: /d\n[e]: /e\n[f]: /f\n"
           "[g]: /g\n[h]: /h\n[i]: /i\n[j]: /j\n\n[a] [b] [c] [i] [j]\n"),
        _T("[x](a&amp;b \"c &amp; d\") ![y](a\\*b \"e &quot; f\")\n"),
        _T("|a|b|\n|-|-|\n|*x*|[y](z)|\n\n- [x] item\n  - nested\n"),
#ifdef MD4C_TEST_LIBREOFFICE
        _T("[[page]] [[page|label &amp; title]]\n\n<div>inline</div>\n"
           "https://example.invalid/ test@example.invalid\n"),
        _T("[a]: /a\n[b]: /b\n[c]: /c\n[d]: /d\n[e]: /e\n[f]: /f\n"
           "[g]: /g\n[h]: /h\n[i]: /i\n[j]: /j\n[k]: /k\n[l]: /l\n"
           "[m]: /m\n[n]: /n\n[o]: /o\n[p]: /p\n[q]: /q\n\n[a] [q]\n"),
#endif
    };
    MD_PARSER parser = {0};
    size_t fixture, rounds = 0;
#ifdef MD4C_TEST_LIBREOFFICE
    /* Match SwMarkdownParser::CallParser(); links are parser text, not opened. */
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_WIKILINKS;
#else
    parser.flags = MD_FLAG_NOHTML | MD_FLAG_NOINDENTEDCODEBLOCKS |
                   MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES | MD_FLAG_TASKLISTS;
#endif
    parser.enter_block = parser.leave_block = block;
    parser.enter_span = parser.leave_span = span;
    parser.text = text;
    for(fixture = 0; fixture < sizeof(inputs) / sizeof(inputs[0]); fixture++) {
        MD_SIZE length = 0;
        size_t allocations, callbacks, i;
        while(inputs[fixture][length]) length++;
        reset(0, 0);
        require(md_parse(inputs[fixture], length, &parser, NULL) == 0, "baseline parse failed");
        require(live_count == 0, "baseline parse leaked memory");
        allocations = allocation_count;
        callbacks = callback_count;
        for(i = 1; i <= allocations; i++) {
            int result;
            reset(i, 0);
            result = md_parse(inputs[fixture], length, &parser, NULL);
            require(injected, "allocation failpoint was not reached");
            require(result != 0, "allocation failure returned success");
            require(live_count == 0, "allocation failure leaked memory");
            rounds++;
        }
        for(i = 1; i <= callbacks; i++) {
            reset(0, i);
            require(md_parse(inputs[fixture], length, &parser, NULL) == 7, "callback abort not propagated");
            require(live_count == 0, "callback abort leaked memory");
            rounds++;
        }
    }
    /* Exercise partial attribute builds and repeated cleanup directly. */
    {
        MD_CTX ctx = {0};
        MD_ATTRIBUTE attr;
        MD_ATTRIBUTE_BUILD build;
        const MD_CHAR raw[] = _T("a&amp;b&quot;c");
        size_t i;
        for(i = 1; i <= 3; i++) {
            reset(i, 0);
            require(md_build_attribute(&ctx, raw, sizeof(raw)/sizeof(raw[0])-1,
                                       0, &attr, &build) != 0, "partial attribute failure returned success");
            md_free_attribute(&ctx, &build);
            md_free_attribute(&ctx, &build);
            require(injected && live_count == 0, "partial attribute cleanup leaked memory");
            rounds++;
        }
    }
    printf("MD4C allocation/callback cleanup regression passed (%zu injected cases).\n", rounds);
    return 0;
}
