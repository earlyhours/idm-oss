# Shared by the application and every test executable using DownloadManager.
IDM_ROOT = $$PWD
isEmpty(CARGO): CARGO = cargo
IDM_IO_LIB = $$IDM_ROOT/build/native-io/release/libidm_native_io.a
native_io.target = $$IDM_IO_LIB
native_io.depends = $$files($$IDM_ROOT/native-io/src/*.rs) $$IDM_ROOT/native-io/Cargo.toml $$IDM_ROOT/native-io/Cargo.lock
native_io.commands = $$CARGO build --release --locked --offline --manifest-path $$shell_quote($$IDM_ROOT/native-io/Cargo.toml) --target-dir $$shell_quote($$IDM_ROOT/build/native-io)
QMAKE_EXTRA_TARGETS += native_io
PRE_TARGETDEPS += $$IDM_IO_LIB
LIBS += $$IDM_IO_LIB -ldl -lpthread -lm
