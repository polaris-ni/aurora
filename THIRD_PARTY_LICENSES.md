# Third-Party Licenses

Aurora 在 `third_party/` 下以**仓库内源码**形式 vendored 以下第三方组件，并在 CMake 中经 `add_subdirectory` 编入静态库 `aurora`，使用源码构建，断网可构建、版本确定。下文逐条列出组件、用途、版本与许可全文索引。

> 集成方式（CMake 顺序）：先 `freetype` 后 `harfbuzz`（`HB_HAVE_FREETYPE` 自动开启），二者 `target_link_libraries(aurora PUBLIC freetype harfbuzz)`。
> FreeType 关闭内置 bzip2/png/HarfBuzz 模块（`FT_DISABLE_BZIP2=ON`、`FT_DISABLE_PNG=ON`、`FT_DISABLE_HARFBUZZ=ON`），shaping 由 aurora 直链 `harfbuzz`；保留 zlib（gzip）解压。

---

## 1. FreeType — 字体栅格化

- **版本**：2.14.3（`third_party/freetype/include/freetype/freetype.h`：`FREETYPE_MAJOR=2` / `MINOR=14` / `PATCH=3`）
- **来源**：vendored 于 `third_party/freetype/`
- **用途**：将字体轮廓栅格化为位图（核心字渲染内核之一）
- **许可**：FreeType License（FTL，GPL 兼容的自由许可），全文见 `third_party/freetype/LICENSE.TXT`
- **内置解压**：FreeType 在 `src/gzip/` 内置 **zlib**（zlib 许可，见该目录 `zlib.h` 头注释）；bzip2/png 内置模块已禁用
- **许可全文**

```
                  FreeType License
                  =================

                          2006-2010, 2012-2014 by
  David Turner, Robert Wilhelm, and Werner Lemberg


This  license is  derived  from the  MIT  X11  license ( Copyright
1996-2000, 2002, 2004  by  David Turner, Robert Wilhelm, and  Werner
Lemberg ) and listed below.

© 1996-2000, 2002, 2004  David Turner, Robert Wilhelm, and Werner Lemberg

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated  documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

  o Redistribution of source code must retain this license file ("LICENSE")
    unaltered; any additions, deletions or changes to the original files
    must be clearly indicated in accompanying documentation.

  o Redistribution in binary form must provide a  copy of this license,
    or at least a pointer to where the license can be obtained if the
    binary is bundled with software that allows the user to reconstruct
    the original source code.

  o The name of the author may not be used to endorse or promote products
    derived from this software without specific prior written permission.

This software is provided "as is" and any warranties, express or implied,
including but not limited to the implied warranties of merchantability and
fitness for a particular purpose, are disclaimed. In no event shall the
author be liable for any direct, indirect, incidental, special, exemplary
or consequential damages (including, but not limited to, procurement of
substitute goods or services; loss of use, data, or profits; or business
interruption) however caused and on any theory of liability, whether in
contract, strict liability, or tort (including negligence or otherwise)
arising in any way out of the use of this software, even if advised of the
possibility of such damage.
```

---

## 2. HarfBuzz — 文本整形（shaping）

- **版本**：14.2.1（`third_party/harfbuzz/src/hb-version.h`：`HB_VERSION_MAJOR=14` / `MINOR=2` / `MICRO=1`）
- **来源**：vendored 于 `third_party/harfbuzz/`
- **用途**：OpenType 整形（GSUB/GPOS/复杂文种与连字/字距/OT 特性），aurora `FontEngine` 经 `hb_ft_font_create` + `hb_shape` 生成 `ShapedGlyph`
- **许可**：Old MIT License，全文见 `third_party/harfbuzz/COPYING`
- **构建范围**：关闭 subset / raster / vector / gpu / utils，仅编核心库（`harfbuzz` 目标）
- **许可全文**

```
Copyright © 2010-2022  Google, Inc.
Copyright © 2015-2020  Ebrahim Byagowi
Copyright © 2019,2020  Facebook, Inc.
Copyright © 2012,2015  Mozilla Foundation
Copyright © 2011  Codethink Limited
Copyright © 2008,2010  Nokia Corporation and/or its subsidiary(-ies)
Copyright © 2009  Keith Stribley
Copyright © 2011  Martin Hosken and SIL International
Copyright © 2007  Chris Wilson
Copyright © 2005,2006,2020,2021,2022,2023  Behdad Esfahbod
Copyright © 2004,2007,2008,2009,2010,2013,2021,2022,2023  Red Hat, Inc.
Copyright © 1998-2005  David Turner and Werner Lemberg
Copyright © 2016  Igalia S.L.
Copyright © 2022  Matthias Clasen
Copyright © 2018,2021  Khaled Hosny
Copyright © 2018,2019,2020  Adobe, Inc
Copyright © 2013-2015  Alexei Podtelezhnikov

HarfBuzz is licensed under the so-called "Old MIT" license.

Permission is hereby granted, without written agreement and without
license or royalty fees, to use, copy, modify, and distribute this
software and its documentation for any purpose, provided that the
above copyright notice and the following two paragraphs appear in
all copies of this software.

IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.

THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
```

---

## 3. zlib — gzip 解压（FreeType 内置）

- **来源**：FreeType 在 `third_party/freetype/src/gzip/` 内置 zlib 源码（`zlib.h` 头注释声明 zlib 许可）
- **用途**：字体文件 gzip 流解压
- **许可**：zlib License（全文见 `third_party/freetype/src/gzip/zlib.h` 顶部注释）

---

## 4. stb_image — 图像加载

- **版本**：单头文件（vendored）
- **来源**：`third_party/stb_image.h`（由 `stb` 项目 vendored）
- **用途**：PNG/JPG 等图像解码（例如 HeadlessSurface 输出、示例图像加载）
- **许可**：Public Domain / MIT（见文件头注释）

---

## 5. nlohmann/json — JSON 解析

- **版本**：单头文件（vendored）
- **来源**：`third_party/nlohmann/json.hpp`
- **用途**：`aurora_api.json` / preferences 等 JSON 读写
- **许可**：MIT License

---

## 6. Noto Sans — 内置默认字体

- **来源**：`src/aurora/text/noto_font_data.cpp` 中的字节数组（由 Noto Sans 字体文件导出）
- **用途**：默认字体 `"sans-serif"` / `"Noto Sans"` / `"default"`，全平台确定性渲染
- **许可**：SIL Open Font License (OFL)

---

## 合规说明

- 上述组件均以源码形式 vendored 于仓库内，可在无网络环境下构建，版本锁定、可审计。
- FreeType（FTL）与 HarfBuzz（Old MIT）、zlib（zlib）、stb_image（Public Domain/MIT）、nlohmann/json（MIT）、Noto Sans（OFL）均为自由/宽松许可，兼容 Aurora 的静态库分发模式。
- 许可全文以各组件目录内原始 `LICENSE.TXT` / `COPYING` / 头注释为权威来源；本文件仅作索引与归档。
