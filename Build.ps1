param(
    [switch]$Asan,
    [switch]$Test,
    [string]$DriverPackageDirectory,
    [string]$BuildDirectory
)
$ErrorActionPreference='Stop'
if(![Environment]::Is64BitProcess){throw 'Use Windows PowerShell x64.'}
if(!$DriverPackageDirectory){$DriverPackageDirectory=Join-Path $PSScriptRoot 'Installer\Driver'}
if(!$BuildDirectory){$BuildDirectory=Join-Path $PSScriptRoot $(if($Asan){'obj\Asan'}else{'obj\Build'})}
$arguments=@('-S',$PSScriptRoot,'-B',[IO.Path]::GetFullPath($BuildDirectory),'-G','Visual Studio 17 2022','-A','x64',('-DHIDACCESS_ASAN='+$(if($Asan){'ON'}else{'OFF'})))
if($DriverPackageDirectory){$arguments+=('-DDRIVER_PACKAGE_DIRECTORY='+[IO.Path]::GetFullPath($DriverPackageDirectory))}
& cmake @arguments
if($LASTEXITCODE){throw 'CMake configuration failed.'}
$hidBuildArguments=@('--build',$BuildDirectory,'--config','Release','--parallel','2')
$hidAsanDirectory=$null
if($Asan){
    # Choose a current ASAN-capable compiler from this same VS installation.
    # Older toolsets can fail while initializing ASAN on current Windows builds.
    $hidCompilerFiles=@(Get-ChildItem -Path (Join-Path $BuildDirectory 'CMakeFiles\*\CMakeCXXCompiler.cmake') -File)
    $hidCompiler=$null
    foreach($hidCompilerFile in $hidCompilerFiles){
        $hidCompilerMatch=Select-String -LiteralPath $hidCompilerFile.FullName -Pattern '^set\(CMAKE_CXX_COMPILER "([^"]+)"\)'
        if($hidCompilerMatch){$hidCompiler=$hidCompilerMatch.Matches[0].Groups[1].Value;break}
    }
    if(!$hidCompiler){throw 'Cannot locate the selected Visual Studio installation for AddressSanitizer.'}
    $hidToolsetRoot=(Get-Item -LiteralPath $hidCompiler).Directory.Parent.Parent.Parent.Parent.FullName
    $hidToolsets=@(Get-ChildItem -LiteralPath $hidToolsetRoot -Directory | Where-Object {
        $_.Name -match '^\d+\.\d+\.\d+$' -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe')) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'))
    } | Sort-Object { [version]$_.Name } -Descending)
    if(!$hidToolsets.Count -or [version]$hidToolsets[0].Name -lt [version]'14.38.0'){
        throw 'AddressSanitizer mode requires a recent Visual Studio 2022 C++ toolset and its C++ AddressSanitizer component. Update Visual Studio and retry.'
    }
    $hidAsanDirectory=Join-Path $hidToolsets[0].FullName 'bin\Hostx64\x64'
    $hidBuildArguments+=@('--',('/p:VCToolsVersion='+$hidToolsets[0].Name))
}
& cmake @hidBuildArguments
if($LASTEXITCODE){throw 'User-mode compilation failed.'}
if($Test){
    $hidSavedTestPath=$env:PATH
    try{
        if($Asan){
            # Only the child test process sees the matching compiler runtime.
            $env:PATH=$hidAsanDirectory+';'+$env:PATH
        }
        & ctest --test-dir $BuildDirectory -C Release --output-on-failure
        if($LASTEXITCODE){throw 'Offline validation failed.'}
    }finally{$env:PATH=$hidSavedTestPath}
}
Write-Output ('Built Windows x64 user-mode tools in '+(Join-Path $BuildDirectory 'bin\Release'))
Write-Output 'No driver was installed and no live policy was enabled.'
