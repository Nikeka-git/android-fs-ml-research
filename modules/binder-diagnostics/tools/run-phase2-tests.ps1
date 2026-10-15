<#
.SYNOPSIS
Builds and runs the standalone Phase 2 POSIX regression tests without Gradle.
.EXAMPLE
.\tools\run-phase2-tests.ps1 -BuildOnly -OutputDirectory C:\work\phase2-tests
.EXAMPLE
.\tools\run-phase2-tests.ps1 -SkipBuild -Serial emulator-5554 -OutputDirectory C:\work\phase2-tests
#>
param(
    [ValidatePattern('^emulator-\d+$')]
    [string] $Serial,
    [string] $SdkPath,
    [string] $OutputDirectory,
    [ValidateNotNullOrEmpty()]
    [ValidateSet('x86_64', 'arm64-v8a')]
    [string[]] $Abis = @('x86_64', 'arm64-v8a'),
    [switch] $BuildOnly,
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
if ($BuildOnly -and $SkipBuild) { throw '-BuildOnly and -SkipBuild cannot be combined.' }
if (-not $BuildOnly -and -not $Serial) { throw '-Serial emulator-NNNN is required when running tests.' }
if (-not $BuildOnly -and $Abis -notcontains 'x86_64') {
    throw 'Running these tests requires x86_64 in -Abis; use -BuildOnly for an ARM-only build.'
}

$projectPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cppPath = Join-Path $projectPath 'app/src/main/cpp'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectPath 'build/phase2-tests' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

if (-not $SdkPath) {
    $propertiesPath = Join-Path $projectPath 'local.properties'
    if (Test-Path -LiteralPath $propertiesPath) {
        foreach ($line in Get-Content -LiteralPath $propertiesPath) {
            if ($line -match '^sdk\.dir=(.+)$') {
                $SdkPath = $Matches[1].Replace('\:', ':').Replace('\\', '\')
                break
            }
        }
    }
    if (-not $SdkPath) { $SdkPath = $env:ANDROID_SDK_ROOT }
    if (-not $SdkPath) { $SdkPath = $env:ANDROID_HOME }
}
if (-not $SdkPath) { throw 'Specify -SdkPath or configure sdk.dir in local.properties.' }

$testCases = @(
    @{ Name = 'phase2_security_tests'; Source = 'phase2_security_test.cpp'; LinkFlags = @() },
    @{
        Name = 'phase2_file_tests'; Source = 'phase2_file_test.cpp'
        LinkFlags = @('-Wl,--wrap=read', '-Wl,--wrap=write', '-Wl,--wrap=readdir', '-Wl,--wrap=fstatat')
    },
    @{ Name = 'phase2_session_tests'; Source = 'phase2_session_test.cpp'; LinkFlags = @() }
)

if (-not $SkipBuild) {
    $buildConfiguration = Get-Content -LiteralPath (Join-Path $projectPath 'app/build.gradle.kts') -Raw
    if ($buildConfiguration -notmatch 'ndkVersion\s*=\s*"([^"]+)"') {
        throw 'ndkVersion was not found in app/build.gradle.kts.'
    }
    $ndkPath = Join-Path $SdkPath ('ndk/' + $Matches[1])
    $toolchainPath = Join-Path $ndkPath 'toolchains/llvm/prebuilt/windows-x86_64'
    $compilerPath = Join-Path $toolchainPath 'bin/clang++.exe'
    $sysrootPath = Join-Path $toolchainPath 'sysroot'
    if (-not (Test-Path -LiteralPath $compilerPath)) { throw "NDK compiler was not found: $compilerPath" }

    foreach ($testCase in $testCases) {
        $sourcePath = Join-Path $cppPath ('tests/' + $testCase.Source)
        if (-not (Test-Path -LiteralPath $sourcePath)) { throw "Test source was not found: $sourcePath" }
    }
    foreach ($abi in ($Abis | Select-Object -Unique)) {
        $target = if ($abi -eq 'x86_64') { 'x86_64-linux-android24' } else { 'aarch64-linux-android24' }
        $abiOutputPath = Join-Path $OutputDirectory $abi
        New-Item -ItemType Directory -Force -Path $abiOutputPath | Out-Null
        foreach ($testCase in $testCases) {
            $sourcePath = Join-Path $cppPath ('tests/' + $testCase.Source)
            $binaryPath = Join-Path $abiOutputPath $testCase.Name
            $compilerArguments = @(
                "--target=$target", "--sysroot=$sysrootPath", '-std=c++17',
                '-Wall', '-Wextra', '-Wformat=2', '-pedantic-errors', '-Werror',
                '-fPIE', '-pie', '-static-libstdc++', '-pthread', '-I', $cppPath
            ) + $testCase.LinkFlags + @($sourcePath, '-o', $binaryPath)
            Write-Output "Building $($testCase.Name) for $abi (Android API 24)"
            & $compilerPath @compilerArguments
            $compilerExitCode = $LASTEXITCODE
            if ($compilerExitCode -ne 0) {
                throw "Building $($testCase.Name) for $abi failed with exit code $compilerExitCode."
            }
        }
    }
}
if ($BuildOnly) {
    Write-Output "Native test binaries are ready in $OutputDirectory"
    return
}

$adbPath = Join-Path $SdkPath 'platform-tools/adb.exe'
if (-not (Test-Path -LiteralPath $adbPath)) { throw "adb was not found: $adbPath" }
foreach ($testCase in $testCases) {
    $binaryPath = Join-Path (Join-Path $OutputDirectory 'x86_64') $testCase.Name
    if (-not (Test-Path -LiteralPath $binaryPath)) { throw "Native test binary was not found: $binaryPath" }
}
$bootCompleted = & $adbPath -s $Serial shell getprop sys.boot_completed
if ($LASTEXITCODE -ne 0 -or "$bootCompleted".Trim() -ne '1') {
    throw "Start and fully boot $Serial before running these tests."
}
$deviceAbi = & $adbPath -s $Serial shell getprop ro.product.cpu.abi
if ($LASTEXITCODE -ne 0 -or "$deviceAbi".Trim() -ne 'x86_64') {
    throw "The selected emulator must use x86_64; reported ABI: $deviceAbi"
}

# Fixed remote root; only this run's three GUID-suffixed executable names are removed.
$remoteRoot = '/data/local/tmp/NativeSimulation-phase2-tests'
$runId = [guid]::NewGuid().ToString('N')
$remoteFiles = [System.Collections.Generic.List[string]]::new()
try {
    & $adbPath -s $Serial shell mkdir -p $remoteRoot
    if ($LASTEXITCODE -ne 0) { throw "Cannot create the native test directory on $Serial." }
    foreach ($testCase in $testCases) {
        $binaryPath = Join-Path (Join-Path $OutputDirectory 'x86_64') $testCase.Name
        $remotePath = $remoteRoot + '/' + $testCase.Name + '-' + $runId
        $remoteFiles.Add($remotePath)
        & $adbPath -s $Serial push $binaryPath $remotePath
        if ($LASTEXITCODE -ne 0) { throw "Pushing $($testCase.Name) to $Serial failed." }
        & $adbPath -s $Serial shell chmod 755 $remotePath
        if ($LASTEXITCODE -ne 0) { throw "Preparing $($testCase.Name) on $Serial failed." }
        Write-Output "Running $($testCase.Name) on $Serial (emulator shell UID)"
        & $adbPath -s $Serial shell env "TMPDIR=$remoteRoot" $remotePath
        $nativeExitCode = $LASTEXITCODE
        if ($nativeExitCode -ne 0) {
            throw "$($testCase.Name) failed with exit code $nativeExitCode."
        }
    }
    Write-Output 'All 3 Phase 2 native test executables passed.'
} finally {
    foreach ($remotePath in $remoteFiles) {
        try {
            & $adbPath -s $Serial shell rm -f -- $remotePath
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "Could not remove test executable $remotePath" -WarningAction Continue
            }
        } catch {
            # A cleanup issue must not replace the original compiler/device/test failure.
            Write-Warning "Could not remove test executable ${remotePath}: $($_.Exception.Message)" -WarningAction Continue
        }
    }
}
