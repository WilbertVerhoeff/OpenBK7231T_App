# Use the bundled ARM linker directly. GCC still supplies the startup objects,
# library paths and standard libraries through its normal link specifications.
override LD = $(CROSS_COMPILE)gcc -fno-use-linker-plugin -specs=$(OBK_DIR)/build_scripts/bk7231n_windows_link.specs
