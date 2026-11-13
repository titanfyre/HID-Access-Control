# HID-Access-Control PowerShell SDK

PowerShell7.4+ Windows x64. `Install-Module HIDAccessControl -Scope CurrentUser`.

```powershell
$client=Connect-HIDAccessControlSDK
try { Invoke-HIDAccessControlSDK -Client $client -Operation status } finally { Disconnect-HIDAccessControlSDK -Client $client }
```

Use -Arguments with a hashtable matching [schema](docs/API.md). Native errors retain their Windows code through the inner exception. Matching driver installed separately.
