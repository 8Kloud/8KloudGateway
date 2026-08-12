# OMT SDK layout

The gateway links only Open Media Transport. It has no NDI dependency or
runtime path. Build OMT and VMX, then lay them out like this:

```text
third_party/omt/
├── include/libomt.h
└── lib/
    ├── libomt.so
    └── libvmx.so
```

The libraries can be built from the MIT-licensed upstream repositories:

```sh
cd third_party
git clone --depth 1 https://github.com/openmediatransport/libvmx
git clone --depth 1 https://github.com/openmediatransport/libomtnet
git clone --depth 1 https://github.com/openmediatransport/libomt

(cd libvmx/build && sh buildlinuxx64.sh)
(cd libomtnet && dotnet build libomtnet.sln -c Release)
(cd libomt && dotnet publish libomt.sln -r linux-x64 -c Release)

mkdir -p omt/include omt/lib
cp libomt/libomt.h omt/include/
cp libomt/bin/Release/net8.0/linux-x64/publish/libomt.so omt/lib/
cp libvmx/build/libvmx.so omt/lib/
```

`libomt.so` loads `libvmx.so` from the same directory. CMake sets the build and
installed runtime paths accordingly and installs both shared libraries into a
private `lib/kloudgateway` directory.
