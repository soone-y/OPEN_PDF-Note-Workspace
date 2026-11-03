int ShortText() {
    const char* text = R"text(short help)text";
    return text[0] ? 1 : 0;
}
