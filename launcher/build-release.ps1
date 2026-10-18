param(
    [string]$Tag = '',
    [switch]$SkipNative,
    [string]$RuntimeVersion = ''
)
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $repo
try {
    $coreVersion = [regex]::Match([System.IO.File]::ReadAllText((Join-Path $repo 'src\version.rc')), 'VALUE "ProductVersion", "([^"]+)"').Groups[1].Value
    if ($coreVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'Cannot read the shared product version.' }
    $launcherVersion = $coreVersion
    if ($Tag -and $Tag -ne "v$coreVersion") { throw "Release tag must match the shared product version: v$coreVersion" }
    if (!$SkipNative) { cmd /c build.bat; if ($LASTEXITCODE) { throw 'Native build failed.' } }
    $payloadVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $repo 'bin\applepie_manager.dll')).ProductVersion
    if ($payloadVersion -ne $coreVersion) { throw "Native payload is $payloadVersion; rebuild it for core $coreVersion before packaging." }
    $output = Join-Path $repo 'bin\release'
    $work = Join-Path $repo ('bin\launcher-build\'+[guid]::NewGuid().ToString('N'))
    $portable = Join-Path $work 'portable'
    $setup = Join-Path $work 'setup'
    New-Item -ItemType Directory -Force -Path $output,$portable,$setup | Out-Null
    $manifest = @{schemaVersion=1;product='ApplepieManager';version=$coreVersion}
    $names = [System.Collections.Generic.List[string]]::new()
    function PublishedFile([string]$name) {
        $path = Join-Path $output $name
        return @{asset=$name;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();size=(Get-Item -LiteralPath $path).Length}
    }
    $publish = @('publish','launcher\Applepie.Launcher.csproj','-c','Release','-r','win-x64','--self-contained','true',"-p:Version=$launcherVersion",'-p:PublishSingleFile=true','-p:IncludeNativeLibrariesForSelfExtract=true','-p:UseSharedCompilation=false','-p:MSBuildEnableWorkloadResolver=false')
    if ($RuntimeVersion) { $publish += "-p:RuntimeFrameworkVersion=$RuntimeVersion" }
    dotnet @publish '-o' $portable
    if ($LASTEXITCODE) { throw 'Launcher publish failed.' }
    $files = @(Get-ChildItem -LiteralPath $portable -Force)
    if ($files.Count -ne 1 -or $files[0].Name -ne 'ApplepieManager.exe' -or $files[0].PSIsContainer) { throw 'Portable output must contain only ApplepieManager.exe.' }
    $desktopVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($files[0].FullName).ProductVersion.Split('+')[0]
    if ($desktopVersion -ne $coreVersion) { throw 'Desktop and DLL versions must match.' }
    $executable = Join-Path $output 'ApplepieManager.exe'
    Copy-Item -LiteralPath $files[0].FullName -Destination $executable -Force
    dotnet @publish "-p:SetupExecutable=$executable" '-o' $setup
    if ($LASTEXITCODE) { throw 'Setup publish failed.' }
    Copy-Item -LiteralPath (Join-Path $setup 'ApplepieManagerSetup.exe') -Destination (Join-Path $output "ApplepieManager-$launcherVersion-win-x64-setup.exe") -Force
    $names.Add('ApplepieManager.exe')
    $names.Add("ApplepieManager-$launcherVersion-win-x64-setup.exe")
    $manifest.desktop = @{version=$launcherVersion;portable=(PublishedFile 'ApplepieManager.exe')}
    $coreFiles = foreach ($dll in @('applepie_manager.dll','d3dcompiler_47.dll','vulkan-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $repo "bin\$dll") -Destination $output -Force
        $names.Add($dll)
        PublishedFile $dll
    }
    $manifest.core = @{version=$coreVersion;files=@($coreFiles)}
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'applepie-release.json') -Encoding utf8
    $names.Add('applepie-release.json')
    $sums = foreach ($name in $names) { (Get-FileHash -LiteralPath (Join-Path $output $name) -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $name }
    [System.IO.File]::WriteAllLines((Join-Path $output 'SHA256SUMS.txt'), $sums, [System.Text.UTF8Encoding]::new($false))
    $names.Add('SHA256SUMS.txt')
    if ($env:GITHUB_OUTPUT) {
        $delimiter = [guid]::NewGuid().ToString('N')
        @("files<<$delimiter") + @($names | ForEach-Object { "bin/release/$_" }) + @($delimiter) | Add-Content -LiteralPath $env:GITHUB_OUTPUT -Encoding utf8
    }
    Write-Host "Release $Tag : ApplepieManager $coreVersion. Artifacts: $output"
}
finally { Pop-Location }
