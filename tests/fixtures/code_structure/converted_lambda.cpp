static int CallbackLeaf(int value) { return value + 1; }
int callback_state = 0;
using Callback = int (*)(int);
Callback registered_callback = nullptr;
void RegisterConvertedLambda() {
    registered_callback = [](int value) -> int {
        try {
            callback_state = value;
            if (value > 0) return CallbackLeaf(value);
            return 0;
        } catch (...) {
            return -1;
        }
    };
}
