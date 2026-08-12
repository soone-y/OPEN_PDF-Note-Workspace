#include <iostream>
#include <filesystem>
int main() {
    std::cout << "Path: " << std::filesystem::temp_directory_path().string() << std::endl;
    return 0;
}
