#!/usr/bin/env python

#--------------------------------------------------------------------------
#
# File:        archive_elf.py
#
# NightDriverStrip - (c) 2026 Plummer's Software LLC.  All Rights Reserved.
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
#    Post-build step: keep a copy of every firmware.elf built, as
#    elf-archive/<env>-<sha256>.elf. A crash report (/api/v1/crash, or
#    "ELF file SHA256" in a serial panic) names its firmware by a prefix of
#    that SHA, and every build embeds its own timestamp, so each build is a
#    unique ELF that a later rebuild would otherwise overwrite - leaving the
#    crash undecodable. Find the match with: ls elf-archive/*-<prefix>*.elf
#
#---------------------------------------------------------------------------

import hashlib
import os
import shutil

Import("env")


def archive_elf(target, source, env):
    elf = target[0].get_abspath()
    with open(elf, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    archive_dir = os.path.join(env.subst("$PROJECT_DIR"), "elf-archive")
    os.makedirs(archive_dir, exist_ok=True)
    destination = os.path.join(archive_dir, f"{env.subst('$PIOENV')}-{sha}.elf")
    if not os.path.exists(destination):
        shutil.copy2(elf, destination)
        print(f"Archived {os.path.basename(destination)}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", archive_elf)
