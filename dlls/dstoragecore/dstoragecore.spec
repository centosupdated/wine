# Wine dstoragecore.dll module definition
# This DLL implements the core DirectStorage runtime:
# - Async I/O via io_uring (Linux native)
# - GPU GDeflate decompression via Vulkan compute (vkd3d-proton)
# - Fence completion signaling
# - Status array tracking
#
# The DLL is a standard Wine builtin PE compiled with MinGW.
# It calls into libds_uring.so (io_uring backend) and libds_gpu.so
# (Vulkan GDeflate compute dispatch) via wine_unix_call.
@ stdcall DStorageGetFactoryCore(ptr ptr)
@ stdcall DStorageSetConfigurationCore(ptr)
@ stdcall DStorageSetConfiguration1Core(ptr)
@ stdcall DStorageCreateCompressionCodecCore(long long ptr ptr)
