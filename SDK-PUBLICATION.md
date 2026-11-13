# SDK publication status

Current source layout and registry coverage. A GitHub download is distinct from publication on a central registry.

| Target | Status | Package / documentation |
| --- | --- | --- |
| CPP/vcpkg | published_custom_registry | hid-access-control |
| CPP/conan | public_recipe_builds_locally |  [Source](packaging/native/) |
| CPP/nuget | paused_pending_owner_email_confirmation | HID-Access-Control.Native |
| NET/nuget | built_verified_publication_on_hold_by_owner | HIDAccessControl.SDK [Source](HID-Access-Control-NET-nuget/) |
| Python/pypi | built_fixture_and_production_verified | hid-access-control [Source](HID-Access-Control-Python-pypi/) |
| Python/conda-forge | built_tested_requires_source_publication_and_official_review | hid-access-control [Source](packaging/conda/) |
| Rust/cargo | published_install_verified | [hid-access-control](https://crates.io/crates/hid-access-control) [Source](HID-Access-Control-Rust-cargo/) |
| JavaScript/npm | awaiting_browser_publication_authentication | [hid-access-control](https://www.npmjs.com/package/hid-access-control) [Source](HID-Access-Control-JavaScript-npm/) |
| Java/maven | published_install_verified | [hid-access-control](https://central.sonatype.com/artifact/io.github.titancsa/hid-access-control/1.0.0) [Source](HID-Access-Control-Java-maven/) |
| Java/jitpack | published_install_verified | [hid-access-control](https://jitpack.io/#titanfyre/HID-Access-Control/java-sdk-v1.0.0) [Source](HID-Access-Control-Java-maven/) |
| Go/modules | published_install_verified | [hid-access-control](https://pkg.go.dev/github.com/titanfyre/HID-Access-Control/HID-Access-Control-Go-modules@v1.0.0) [Source](HID-Access-Control-Go-modules/) |
| Go/pkg-go-dev | indexed_verified | [Documentation](https://pkg.go.dev/github.com/titanfyre/HID-Access-Control/HID-Access-Control-Go-modules@v1.0.0) |
| PowerShell/gallery | published_install_verified | [HIDAccessControl](https://www.powershellgallery.com/packages/HIDAccessControl/1.0.0) [Source](HID-Access-Control-PowerShell-gallery/) |
| Dart/pub-dev | built_fixture_and_production_verified | hid_access_control [Source](HID-Access-Control-Dart-pub-dev/) |
| Ruby/rubygems | published_install_verified | [hid-access-control](https://rubygems.org/gems/hid-access-control) [Source](HID-Access-Control-Ruby-rubygems/) |
| PHP/packagist | composer_source_and_tag_published | [titancsa/hid-access-control](https://packagist.org/packages/titancsa/hid-access-control) |

Native SDK source and CMake/Conan/vcpkg/NuGet recipes share [`packaging/native`](packaging/native/LAYOUT.md). Previous release archives and tags remain unchanged.

Application and driver prerequisites and original licensing remain those documented by each SDK. No driver installer is bundled with these source packages.
