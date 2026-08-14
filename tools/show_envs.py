#!/usr/bin/env python

#--------------------------------------------------------------------------
#
# File:        show_envs.py
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
#    This script outputs a JSON array with all the environment names defined in
#    platformio.ini.
#    It is used by this project's GitHub CI workflow.
#
#    The script resolves platformio.ini relative to its own location, so it can
#    be executed from any working directory.
#
# History:     Aug-08-2023         Rbergen      Added header
#              Aug-27-2023         Rbergen      Make importable
#
#---------------------------------------------------------------------------

import configparser
import json
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
PLATFORMIO_INI = PROJECT_ROOT / 'platformio.ini'


def getenvs(config_path=PLATFORMIO_INI):
    config = configparser.ConfigParser()
    with Path(config_path).open(encoding='utf-8') as config_file:
        config.read_file(config_file)

    envs = []

    for section in config.sections():
        if section.startswith('env:') and len(section) > 4:
            envs.append(section[4::])

    return envs

if __name__ == '__main__':
    print(json.dumps(getenvs()))
