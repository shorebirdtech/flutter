# Shorebird's change in the Flutter Repo 🐦

This repository is the fork of the [Flutter](https://github.com/flutter/flutter) repository
with the changes necessary to support [Shorebird](https://shorebird.dev).

This document lists and explains these changes.

## Flutter Tools

The [Flutter Tools](./packages/flutter_tools) contains code that might alter the `shorebird.yaml`
file originally created by the user.

Changes on the file are done only on bundled files, and the user's file is not altered.

The changes are:

> `app_id` when using Flavors

When using Flavors, a `shorebird.yaml` might look like this:

```yaml
app_id: 1
flavors:
  global: 2
  internal: 3
```

When the user executes a shorebird release/patch command, using a flavor, the file will be updated
to set the app id to the flavor's app id, so if using the `internal` flavor, the file will look like this:

```yaml
app_id: 3
```

You can find these customizations in the following source files:
 - [Android](https://github.com/shorebirdtech/flutter/blob/shorebird/dev/packages/flutter_tools/gradle/src/main/groovy/flutter.groovy#L1337)
 - [iOS](https://github.com/shorebirdtech/flutter/blob/shorebird/dev/packages/flutter_tools/lib/src/ios/mac.dart#L563)

> Adds `patch_public_key` when a key encoded key is provied

When an encoded public key is provided via the `SHOREBIRD_PUBLIC_KEY` environment variable. The
`shorebird.yaml` file will be updated to include the `patch_public_key` attribute containing the
value received.

This is done just on Android at this moment.

You can find these customizations in the following source files:
 - [Android](https://github.com/shorebirdtech/flutter/blob/shorebird/dev/packages/flutter_tools/gradle/src/main/groovy/flutter.groovy#L1339)
