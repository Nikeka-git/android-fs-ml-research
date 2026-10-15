param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^emulator-\d+$')]
    [string] $Serial,
    [string] $SdkPath,
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
$projectPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

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

$adbPath = Join-Path $SdkPath 'platform-tools/adb.exe'
if (-not (Test-Path -LiteralPath $adbPath)) { throw "adb was not found at $adbPath" }
$bootCompleted = & $adbPath -s $Serial shell getprop sys.boot_completed
if ($LASTEXITCODE -ne 0 -or "$bootCompleted".Trim() -ne '1') {
    throw "Start and fully boot the emulator $Serial before running these tests."
}

Push-Location -LiteralPath $projectPath
try {
    if (-not $SkipBuild) {
        & (Join-Path $projectPath 'gradlew.bat') ':app:assembleDebug' ':app:assembleDebugAndroidTest'
        if ($LASTEXITCODE -ne 0) { throw 'Building the app and instrumentation APK failed.' }
    }

    $apkPaths = @(
        (Join-Path $projectPath 'app/build/outputs/apk/debug/app-debug.apk'),
        (Join-Path $projectPath 'app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk')
    )
    foreach ($apkPath in $apkPaths) {
        if (-not (Test-Path -LiteralPath $apkPath)) { throw "APK was not found: $apkPath" }
        & $adbPath -s $Serial install -r -t $apkPath
        if ($LASTEXITCODE -ne 0) { throw "Installing $apkPath on $Serial failed." }
    }

    # Explicit serial selection keeps an attached physical phone outside the test run.
    $output = & $adbPath -s $Serial shell am instrument -w `
        'com.example.simulation.production.test/android.test.InstrumentationTestRunner' 2>&1
    $exitCode = $LASTEXITCODE
    $output | Write-Output
    $report = $output -join "`n"
    # adb can exit successfully even if the instrumentation itself failed.
    if ($exitCode -ne 0 -or $report -notmatch 'OK\s*\([1-9]\d* tests?\)' `
        -or $report -match 'FAILURES!!!|INSTRUMENTATION_FAILED|INSTRUMENTATION_ABORTED|shortMsg=') {
        throw 'Instrumentation tests failed. See the runner output above.'
    }
} finally {
    Pop-Location
}
