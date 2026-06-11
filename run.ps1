# VortexOS QEMU Launch Script
# Run: powershell -ExecutionPolicy Bypass .\run.ps1

$VORTEX_ROOT = "G:\VortexOS"
$KERNEL = "$VORTEX_ROOT\build\vortexos.elf"
$QEMU = "qemu-system-x86_64"

if (!(Test-Path $KERNEL)) {
    Write-Host "Kernel not found. Building first..."
    Set-Location $VORTEX_ROOT
    & .\build.ps1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build failed!" -ForegroundColor Red
        exit 1
    }
}

Write-Host "Starting VortexOS in QEMU..." -ForegroundColor Green

& $QEMU `
    -machine q35 `
    -cpu host `
    -m 512M `
    -smp 4 `
    -kernel $KERNEL `
    -serial stdio `
    -no-reboot `
    -d int,cpu_reset `
    -D qemu.log `
    -drive file=fat:rw:esp,format=raw `
    -netdev user,id=net0 `
    -device virtio-net,netdev=net0
