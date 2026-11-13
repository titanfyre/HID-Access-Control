# HID-Access-Control Python SDK

`pip install hid-access-control`

Windows x64 Python3.10+. No driver is installed by this package.

```python
from hid_access_control import Client
with Client() as client:
    info = client.call("status")
```

[Operation schema and API](docs/API.md). Check errors and the product licensing state before device creation.
