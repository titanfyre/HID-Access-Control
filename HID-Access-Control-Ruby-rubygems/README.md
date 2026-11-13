# HID-Access-Control Ruby SDK

`gem install hid-access-control`

Windows x64 Ruby3.1+.

```ruby
require "hid_access_control"
HIDAccessControlSDK::Client.open { |client| p client.call("status") }
```

[API and operation schema](docs/API.md). Matching driver installed separately.
