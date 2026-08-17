# Wine dstorage.dll module definition
# This PE DLL shims the DirectStorage API for Wine/Proton.
# It forwards calls to the native libdstorage.so via dlopen/dlsym.
@ stdcall DStorageSetConfiguration(ptr)
@ stdcall DStorageSetConfiguration1(ptr)
@ stdcall DStorageGetFactory(ptr ptr)
@ stdcall DStorageCreateCompressionCodec(long long ptr ptr)
