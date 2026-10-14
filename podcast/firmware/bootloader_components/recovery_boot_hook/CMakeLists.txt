# Normalize path separators: $ENV{IDF_PATH} contains backslashes on Windows,
# which CMake rejects as invalid escape sequences in include dirs below.
set(_recovery_idf_path "$ENV{IDF_PATH}")
string(REPLACE "\\" "/" _recovery_idf_path "${_recovery_idf_path}")

idf_component_register(
    SRCS "recovery_boot_hook.c"
    PRIV_REQUIRES bootloader_support hal esp_rom
    PRIV_INCLUDE_DIRS "${_recovery_idf_path}/components/bootloader_support/private_include"
                      "${CMAKE_CURRENT_LIST_DIR}/../../components/bsp/include"
)
