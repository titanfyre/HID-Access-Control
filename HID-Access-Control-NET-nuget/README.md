# HID-Access-Control .NET8 SDK

Package ID `HIDAccessControl.SDK`. NuGet upload remains on hold at owner request.

```csharp
using var client=new TitanCSA.Products.HIDAccessControl.Client();
string statusJson=client.Call("status", "{}");
```

Client implements IDisposable; Call takes raw argument JSON object and returns result JSON. NativeException exposes Code. UInt64 fields remain decimal strings. [API](docs/API.md). Windows x64 and separately installed matching driver required.
