#!/usr/bin/env python

#--------------------------------------------------------------------------
#
# File:        bake_site.py
#
# NightDriverStrip - (c) 2023 Plummer's Software LLC.  All Rights Reserved.
#
# This file is part of the NightDriver software project.
#
#    NightDriver is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    NightDriver is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with Nightdriver.  It is normally found in copying.txt
#    If not, see <https://www.gnu.org/licenses/>.
#
# Description:
#
#    Script to bake the on-board web application ("site")
#
# History:     Aug-08-2023         Rbergen      Added header
#
#---------------------------------------------------------------------------

import gzip
import os
import shutil


SITE_DIR = "site"
DEST_DIR = os.path.join(SITE_DIR, "dist")
ASSETS = (
    ("index.html", "index.html.gz"),
    ("styles.css", "styles.css.gz"),
    ("app.js", "app.js.gz"),
)


def gzip_file(source_path, dest_path):
    with open(source_path, "rb") as src, open(dest_path, "wb") as raw_dst:
        with gzip.GzipFile(filename="", mode="wb", compresslevel=9, fileobj=raw_dst) as dst:
            shutil.copyfileobj(src, dst)


os.makedirs(DEST_DIR, exist_ok=True)

total_bytes = 0
for source_name, dest_name in ASSETS:
    source_path = os.path.join(SITE_DIR, source_name)
    dest_path = os.path.join(DEST_DIR, dest_name)
    gzip_file(source_path, dest_path)
    size = os.stat(dest_path).st_size
    total_bytes += size
    print(f"Baked {source_name} -> {dest_name}: {size} B")

print(f"Build completed, total: {total_bytes / 1024:.1f} KB")
