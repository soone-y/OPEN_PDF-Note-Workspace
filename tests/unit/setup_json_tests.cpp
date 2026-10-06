// Appended to the actual production helpers by test_setup_json_roundtrip.py.
// Every file operation is confined to the runner's disposable fixture.
static void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

static std::vector<std::wstring> FixtureDirs(const std::filesystem::path& exeDir) {
    return {
        (exeDir.parent_path() / L"external" / L"授業 資料" / L"notes").wstring(),
        (exeDir / L"relative" / L"授業 ] $& $1 $$ ' 資料").wstring(),
        (exeDir.parent_path() / L"external" / L"tab" / L"new" / L"back").wstring(),
        (exeDir.parent_path() / L"external" / (std::wstring(100, L'長') + L" 😀")).wstring(),
    };
}

int main(int argc, char** argv) {
    Expect(argc == 2, "mode required");
    const auto exeDir = CurrentExecutableDirectoryForSetup();
    Expect(exeDir.has_value(), "executable directory unavailable");
    const auto setup = ResolveSetupJsonPath(*exeDir);
    const auto dirs = FixtureDirs(*exeDir);
    const std::string mode = argv[1];
    if (mode == "write") {
        for (const auto& dir : dirs) {
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            Expect(!error, "fixture directory creation failed");
        }
        std::string json = R"({"workspaceRoot":"workspace","readonlyViewer":{"sentinel":"keep $& ]"},"annotToolModeOrder":["select","pan"]})";
        const auto array = BuildSetupPathArrayJson(*exeDir, dirs);
        Expect(ReplaceOrInsertJsonArrayFieldAfter(json, "tempExternalLectureDirs", array, "workspaceRoot"), "insert failed");
        Expect(AtomicWriteSetupJsonIfValid(setup, json), "save failed");
        Expect(ParseJsonStringArrayField(json, "tempExternalLectureDirs").size() == dirs.size(), "array invalid");
    } else if (mode == "read") {
        // Separate process: exercises the app's actual setup loader after restart.
        const auto loaded = LoadSetupTempExternalLectureDirs();
        Expect(loaded == dirs, "saved paths changed after restart");
    } else if (mode == "update") {
        std::string json = ReadTextFileUtf8(setup);
        const std::string newRoot = WideToUTF8((exeDir->parent_path() / L"workspace ] $&" / L"授業").wstring());
        Expect(ReplaceOrInsertJsonStringField(json, "workspaceRoot", newRoot), "root replacement failed");
        Expect(ReplaceOrInsertJsonStringFieldAfter(json, "workspaceRootMode", "absolute", "workspaceRoot"), "mode insert failed");
        Expect(ParseJsonStringField(json, "workspaceRoot") == newRoot, "root escape roundtrip failed");
        auto shortened = dirs;
        shortened.pop_back();
        Expect(ReplaceOrInsertJsonArrayFieldAfter(json, "tempExternalLectureDirs", BuildSetupPathArrayJson(*exeDir, shortened), "workspaceRoot"), "remove path failed");
        Expect(ReplaceOrInsertJsonArrayFieldAfter(json, "tempExternalLectureDirs", BuildSetupPathArrayJson(*exeDir, dirs), "workspaceRoot"), "re-add path failed");
        Expect(!ReplaceOrInsertJsonArrayFieldAfter(json, "tempExternalLectureDirs", BuildSetupPathArrayJson(*exeDir, dirs), "workspaceRoot"), "identical update changed JSON");
        Expect(AtomicWriteSetupJsonIfValid(setup, json), "update save failed");
        // An escaped quote is legal JSON even though Windows disallows it in names.
        std::string quoted = R"({"workspaceRoot":"a\"b","annotToolModeOrder":["select"]})";
        Expect(ReplaceOrInsertJsonArrayFieldAfter(quoted, "tempExternalLectureDirs", "[\"a\\\"b\",\"c]d\"]", "workspaceRoot"), "escaped anchor insert failed");
        Expect(IsSyntacticallyValidJsonLite(quoted), "escaped anchor corrupted JSON");
        Expect(ParseJsonStringArrayField(quoted, "tempExternalLectureDirs") == std::vector<std::string>({"a\"b", "c]d"}), "escaped array decode failed");
        Expect(ParseJsonStringArrayField(R"({"tempExternalLectureDirs":["valid",42]})", "tempExternalLectureDirs").empty(), "partial malformed array accepted");
        Expect(BuildSetupPathArrayJson(*exeDir, {L"\\\\server\\share", L"\\\\?\\C:\\device"}) == "[]", "network/device paths persisted");
    } else if (mode == "strings") {
        std::string controls;
        for (int code = 0; code < 32; ++code) controls.push_back(static_cast<char>(code));
        const std::vector<std::string> values = {
            WideToUTF8(L"日本語 😀 { } [ ] $& $1"),
            "C:\\notes\\tab\\back", "quote\"and\\slash", "line\nnext\r\tend",
            std::string("zero\0one\x01two", 12), controls, "",
        };
        for (const auto& value : values) {
            const std::string quoted = "\"" + EscapeJsonStringValue(value) + "\"";
            std::string decoded = "unchanged";
            size_t cursor = 0;
            Expect(ParseJsonStringToken(quoted, &cursor, &decoded) && decoded == value && cursor == quoted.size(), "string roundtrip lost data");
            // The read-only viewer's writer uses Unicode escapes for controls.
            const std::string viewerJson = "{\"path\":\"" + EscapeJsonString(value) + "\"}";
            std::wstring loaded;
            Expect(JsonStringMember(viewerJson, "path", &loaded) && loaded == UTF8ToWide(value), "viewer string roundtrip failed");
        }
        const std::string escaped = R"({"workspaceRoot":"\u65e5\u672c\u8a9e \uD83D\uDE00"})";
        const std::string expected = WideToUTF8(L"日本語 😀");
        Expect(ParseJsonStringField(escaped, "workspaceRoot") == expected, "Unicode escapes were not decoded");
        const std::string boundary = R"("\u007f\u0080\u07ff\u0800\uffff\uD800\uDC00\uDBFF\uDFFF")";
        const std::wstring boundaryExpected = {0x7f, 0x80, 0x7ff, 0x800, 0xffff, 0xd800, 0xdc00, 0xdbff, 0xdfff};
        size_t boundaryPos = 0;
        std::string boundaryDecoded;
        Expect(ParseJsonStringToken(boundary, &boundaryPos, &boundaryDecoded) &&
               boundaryDecoded == WideToUTF8(boundaryExpected), "Unicode UTF-8 boundaries decoded incorrectly");
        const std::string imported = R"({"nested":{"classesDir":"wrong"},"classesDir":"\u65e5\u672c\u8a9e \\\"[]"})";
        Expect(ExtractSettingsJsonStringField(imported, "classesDir") == L"日本語 \\\"[]", "import path escapes or top-level lookup failed");
        std::wstring viewerValue;
        Expect(JsonStringMember(escaped, "workspaceRoot", &viewerValue) && viewerValue == L"日本語 😀", "viewer Unicode escapes rejected");
        std::string nested = R"({"readonlyViewer":{"workspaceRoot":"wrong"},"workspaceRoot":"right"})";
        Expect(ParseJsonStringField(nested, "workspaceRoot") == "right", "nested field shadowed root");
        std::string mismatch = R"({"other":"old","classesDir":"old"})";
        Expect(ReplaceOrInsertJsonStringField(mismatch, "classesDir", "new\\path") &&
               ParseJsonStringField(mismatch, "other") == "old" &&
               ParseJsonStringField(mismatch, "classesDir") == "new\\path", "class directory update changed unrelated content");
        for (const auto& invalid : {R"("\u12G4")", R"("\ud800")", R"("\udc00")", R"("\ud800\u0041")", "\"raw\nline\"", "\"unterminated"}) {
            size_t pos = 0;
            std::string output = "retained";
            Expect(!ParseJsonStringToken(invalid, &pos, &output) && pos == 0 && output == "retained", "invalid string partially changed output");
            Expect(!IsSyntacticallyValidJsonLite(std::string("{\"workspaceRoot\":") + invalid + "}"), "invalid string accepted as JSON");
            Expect(!IsSettingsJsonObjectSyntaxValid(std::string("{\"classesDir\":") + invalid + "}"), "invalid settings string accepted");
        }
        ThemeColors theme;
        theme.name = L"name \"quoted\" \\ { } ] 😀";
        theme.nameJp = L"日本語 \n\tテーマ";
        std::ostringstream themeJson;
        themeJson << "{\n";
        WriteThemeObject(themeJson, "  ", theme);
        themeJson << "}\n";
        Expect(IsSyntacticallyValidJsonLite(themeJson.str()), "theme writer produced invalid JSON");
        Expect(ParseJsonStringField(themeJson.str(), "name") == WideToUTF8(theme.name) &&
               ParseJsonStringField(themeJson.str(), "name_jp") == WideToUTF8(theme.nameJp), "theme names lost data");
        const std::string metadata = "{\"verified\":[{\"file\":\"theme_00AA7B.json\",\"display\":\"" +
            EscapeJsonStringValue(WideToUTF8(theme.name)) + "\"}]}";
        const auto verified = ParseVerifiedThemes(metadata);
        Expect(verified.size() == 1 && verified[0].displayName == theme.name, "theme cache dropped escaped/bracketed names");
        g_themeVerified = verified;
        g_themeVerified[0].sha256 = "edited \" cache value";
        const auto themeFile = *exeDir / L"theme_fixture.json";
        WriteThemeConfig(themeFile, exeDir->wstring(), verified[0].file, {});
        const auto cached = ParseVerifiedThemes(ReadTextFileUtf8(themeFile));
        Expect(cached.size() == 1 && cached[0].displayName == theme.name &&
               cached[0].sha256 == g_themeVerified[0].sha256, "theme metadata writer lost data");
        WorkspaceConfig schedule;
        schedule.schedulePeriods = 2;
        schedule.scheduleCells = {L"日本語 \"引用\" \\ ] } $&\n改行 😀"};
        schedule.scheduleStartTimes = {L"09:00", L"text \" \\ \t end"};
        SaveScheduleStartTimes(*exeDir, schedule);
        WorkspaceConfig restored;
        restored.schedulePeriods = schedule.schedulePeriods;
        Expect(LoadScheduleStartTimes(*exeDir, restored), "schedule reload failed");
        Expect(restored.scheduleCells[0] == schedule.scheduleCells[0] &&
               restored.scheduleStartTimes[1] == schedule.scheduleStartTimes[1], "schedule strings lost data");
    } else if (mode == "reject") {
        const auto before = ReadTextFileUtf8(setup);
        Expect(!AtomicWriteSetupJsonIfValid(setup, R"({"workspaceRoot":"C:\invalid\path"})"), "invalid JSON accepted");
        Expect(ReadTextFileUtf8(setup) == before, "invalid JSON damaged original");
        Expect(!AtomicWriteSetupJsonIfValid(setup, R"({"workspaceRoot":"workspace","futureField":true})"), "unknown field accepted");
        Expect(ReadTextFileUtf8(setup) == before, "unknown field damaged original");
        std::string replacement = before;
        Expect(ReplaceOrInsertJsonArrayField(replacement, "tempExternalLectureDirs", "[]"), "failure fixture unchanged");
        HANDLE locked = CreateFileW(setup.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Expect(locked != INVALID_HANDLE_VALUE, "lock fixture failed");
        const bool saved = AtomicWriteSetupJsonIfValid(setup, replacement);
        CloseHandle(locked);
        Expect(!saved, "locked file incorrectly saved");
        Expect(ReadTextFileUtf8(setup) == before, "locked write damaged original");
    } else if (mode == "remove") {
        auto json = ReadTextFileUtf8(setup);
        Expect(ReplaceOrInsertJsonArrayFieldAfter(json, "tempExternalLectureDirs", "[]", "workspaceRoot"), "clear failed");
        Expect(AtomicWriteSetupJsonIfValid(setup, json), "clear save failed");
    } else if (mode == "empty") {
        Expect(LoadSetupTempExternalLectureDirs().empty(), "cleared paths returned after restart");
    } else {
        Expect(false, "unknown mode");
    }
    return 0;
}
