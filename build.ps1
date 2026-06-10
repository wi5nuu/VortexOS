$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
Set-Location G:\VortexOS
if (Test-Path build) { Remove-Item -Recurse -Force build }
$toolchain = (Resolve-Path "cmake/x86_64-toolchain.cmake").Path -replace '\\','/'
& cmake -B build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DCMAKE_BUILD_TYPE=Debug"
& cmake --build build
