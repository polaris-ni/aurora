# Aurora 错误码目录（自动生成）

> 由 `tools/gen_error_codes.cpp` 从 `codespec/errors.toml` 生成，**请勿手改**。
> `slug` 为冻结对外契约，跨语言/JSON/日志只认它；`enum` 为 C++ 标识符，可自由改名。

| # | enum | slug | category | severity | auto_fixable | fix_category | retryable | message | hint |
|---|------|------|----------|----------|--------------|--------------|-----------|---------|------|
| 0 | `GeneralUnknown` | `general-unknown` | general | error | false | unknown | false | An unknown error occurred | Check the log context or enable the diagnostic collector |
| 1 | `GeneralInvalidArgument` | `general-invalid-argument` | general | error | false | invalid_value | false | Invalid argument supplied | Check the parameter type and value range |
| 2 | `GeneralNotSupported` | `general-not-supported` | general | error | false | unknown | false | Operation not supported | Confirm whether the feature is available or use an alternative |
| 3 | `LayoutNullChild` | `layout-null-child` | layout | warning | true | missing_child | false | Layout child widget is null | Remove the empty child node or provide a valid widget |
| 4 | `LayoutDepthExceeded` | `layout-depth-exceeded` | layout | warning | true | layout_conflict | false | Layout tree depth exceeded the limit (default {max}) | Check for infinitely recursive Repeater/Provider |
| 5 | `LayoutInvalidConstraints` | `layout-invalid-constraints` | layout | warning | true | invalid_value | false | Invalid constraints (e.g. min > max) | Check the constraints settings |
| 6 | `LayoutSizeOutOfConstraints` | `layout-size-out-of-constraints` | layout | warning | true | invalid_value | false | Computed size out of constraints range | Adjust the min/max constraints or content size |
| 7 | `WidgetUnknownType` | `widget-unknown-type` | widget | warning | true | unknown_widget | false | Unregistered widget type '{type}' encountered during deserialization | Register it first via WidgetRegistry::register_factory |
| 8 | `WidgetInvalidProp` | `widget-invalid-prop` | widget | warning | true | invalid_value | false | Invalid property value: '{prop}' | Check the property type and value range |
| 9 | `WidgetPropConstraintViolated` | `widget-prop-constraint-violated` | widget | warning | true | invalid_value | false | Property constraint violated (mutually exclusive properties set together) | Check the validity of the property combination |
| 10 | `WidgetMissingRequiredProp` | `widget-missing-required-prop` | widget | warning | true | missing_prop | false | Missing required property '{prop}' | Add the missing required property |
| 11 | `RenderFontLoadFailed` | `render-font-load-failed` | render | error | false | resource_error | false | Font load failed: '{path}' | Check the font path and format |
| 12 | `RenderImageLoadFailed` | `render-image-load-failed` | render | error | false | resource_error | false | Image load failed: '{path}' | Check the image path, format and decoder |
| 13 | `RenderInvalidColor` | `render-invalid-color` | render | error | false | invalid_value | false | Invalid color value (e.g. component out of 0-255) | Check the color component values |
| 14 | `IOFileNotFound` | `io-file-not-found` | io | error | false | resource_error | false | File does not exist or cannot be opened: '{path}' | Check the file path and permissions |
| 15 | `IOParseFailed` | `io-parse-failed` | io | error | false | type_error | false | File parsing failed: '{detail}' | Check the file content format |
| 16 | `IOImageDecodeFailed` | `io-image-decode-failed` | io | error | false | resource_error | false | Image decode failed | Check image file validity or use a supported format |
| 17 | `IOImageInvalidDimensions` | `io-image-invalid-dimensions` | io | error | false | invalid_value | false | Invalid image dimensions (width/height is 0 or exceeds the limit) | Check image dimensions or use a valid-size resource |
| 18 | `ValidationFailed` | `validation-failed` | validation | warning | true | type_error | false | Validation failed: '{detail}' | Check input data validity |
| 19 | `ValidationSchemaMismatch` | `validation-schema-mismatch` | validation | warning | true | type_error | false | Schema mismatch | Check data structure against the Schema definition |
| 20 | `ValidationTreeTooDeep` | `validation-tree-too-deep` | validation | warning | true | layout_conflict | false | UI tree depth exceeded the limit (default {max}) | Reduce nesting levels or split components |
| 21 | `ValidationUnknownWidget` | `validation-unknown-widget` | validation | warning | true | unknown_widget | false | Unknown/unsupported widget type '{type}' | Use a known widget type or register a custom type |
| 22 | `ValidationNullChild` | `validation-null-child` | validation | warning | true | missing_child | false | Child node is null | Provide a valid child node or remove the item |
| 23 | `NavDepthExceeded` | `nav-depth-exceeded` | navigation | error | false | layout_conflict | false | Navigation stack depth exceeded the limit (default {max}) | Check the Navigator push loop logic or call set_max_depth() |
| 24 | `NavRouteNotFound` | `nav-route-not-found` | navigation | error | false | missing_prop | false | Route not registered: '{route}' | Register the corresponding route or use the default route |
| 25 | `PlatformUnavailable` | `platform-unavailable` | platform | error | false | resource_error | false | Platform feature unavailable: '{detail}' | Check the runtime environment or enable the corresponding backend |
| 26 | `PlatformPermissionDenied` | `platform-permission-denied` | platform | error | false | resource_error | false | Insufficient permissions: '{detail}' | Check file/network permission settings |
| 27 | `PlatformComInitFailed` | `platform-com-init-failed` | platform | error | false | resource_error | false | COM initialization failed | Initialize COM outside the GUI thread or check the runtime environment |
| 28 | `PlatformDialogCreateFailed` | `platform-dialog-create-failed` | platform | error | false | resource_error | false | Platform dialog creation failed | Check the parent window handle or runtime environment |
| 29 | `RuntimeCoroutineException` | `runtime-coroutine-exception` | runtime | error | false | exception | true | Exception thrown during coroutine execution: '{detail}' | Check the coroutine body for exception catching or fix the logic |
| 30 | `RuntimeAsyncException` | `runtime-async-exception` | runtime | error | false | exception | true | Exception thrown during async task execution: '{detail}' | Check the async callback for exception catching or fix the logic |
| 31 | `RuntimeAsyncTimeout` | `async-timeout` | runtime | error | false | timeout | true | Async task timed out (not completed within {ms}ms) | Increase the timeout threshold or optimize async task duration |
| 32 | `GenerateUiEmpty` | `generation-ui-empty` | generation | error | false | empty | false | NL-to-UI generation result is empty (no valid widget) | Provide a more specific natural language description or constraints |
| 33 | `RendererUnavailable` | `renderer-unavailable` | platform | error | false | resource_error | false | Requested render backend is unavailable | Check the backend compile switch (AURORA_BACKEND_D3D11) or fall back to software rendering (RendererPreference::Auto/Software) |
| 34 | `PrefsNotPersistent` | `prefs-not-persistent` | platform | error | false | resource_error | false | Preferences is in memory-only mode and cannot be persisted | Bind a config file path (Preferences(file)) to gain persistence |
| 35 | `PrefsOpenFailed` | `prefs-open-failed` | platform | error | false | resource_error | false | Failed to open config file: '{path}' | Check the file path and read permissions |
| 36 | `PrefsParseFailed` | `prefs-parse-failed` | platform | error | false | type_error | false | Config file JSON parse failed: '{detail}' | Check JSON syntax (quotes/commas/brackets) and field types |
| 37 | `PrefsWriteFailed` | `prefs-write-failed` | platform | error | false | resource_error | false | Config file write failed: '{detail}' | Check disk space, whether the directory exists, and target file permissions |
| 38 | `WidgetDepthExceeded` | `widget-depth-exceeded` | widget | warning | true | layout_conflict | false | Widget tree depth exceeded the limit (default 64) | Usually caused by infinitely recursive Repeater / self-referencing Provider; check rebuild conditions or set_max_depth() |
| 39 | `RenderDegraded` | `render-degraded` | render | warning | false | invalid_value | false | Rendering degraded due to missing resources (e.g. font/image fallback) | Non-fatal degradation, UI remains usable; check the missing resource path or provide alternatives |
| 40 | `FontMissing` | `font-missing` | render | warning | false | resource_error | false | Requested font unavailable, fell back to built-in Bitmap font | On non-Windows platforms the GDI font fallback chain may differ; ensure the font is registered or use the built-in font |
| 41 | `SurfaceLost` | `surface-lost` | platform | warning | false | resource_error | false | Surface lost (e.g. window destroyed/device reset) | Need to rebuild the Surface to continue rendering; in most cases the framework auto-recovers on next present |
| 42 | `NotRestorable` | `not-restorable` | widget | warning | false | unknown | false | Widget not restorable (skipped during serialization/deserialization) | Widgets relying on runtime callbacks such as Repeater / Canvas cannot be restored from JSON; use a named factory or State injection |
| 43 | `JsonParseError` | `json-parse-error` | io | error | false | type_error | false | JSON parse failed | Check JSON syntax (quotes/commas/brackets) and whether field types match the Schema |
| 44 | `StorageBackendUnavailable` | `storage-backend-unavailable` | io | error | false | resource_error | false | Storage backend unavailable (directory not writable or media open failed) | Check storage path permissions; when a cross-process lock is held, wait or close the occupying process |
| 45 | `StorageRecordNotFound` | `storage-record-not-found` | io | error | false | missing_child | false | Storage record does not exist | Confirm the id spelling; or put first then get |
| 46 | `StorageRecordCorrupt` | `storage-record-corrupt` | validation | error | false | type_error | false | Storage record corrupt (JSON parse failed or illegal structure) | Delete the corrupt file or restore from backup; use clear to rebuild if necessary |
| 47 | `StorageTypeMismatch` | `storage-type-mismatch` | validation | error | false | type_error | false | Typed read type mismatch | Ensure put/get use the same type T; or use get_value to read the raw payload |
| 48 | `StorageEncodingMismatch` | `storage-encoding-mismatch` | validation | error | false | type_error | false | Serialized wire format mismatch | Record stored as binary but type T only supports JSON (or vice versa); use get_value or fix T's serialization concept |
| 49 | `StorageIoError` | `storage-io-error` | io | error | false | resource_error | false | Storage underlying I/O failed | Check disk space/permissions; rename failure may be a cross-volume move, use a same-volume path |

<!-- 计数：共 50 条错误码 -->
