#!/usr/bin/env python

#--------------------------------------------------------------------------
#
# File:        build_all.py
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
#    This script builds all the environment names defined in platformio.ini.
#
#    The script can be executed from any working directory.
#
# History:     Aug-27-2023         Rbergen      Created
#
#---------------------------------------------------------------------------

import subprocess


if __package__:
    from . import show_envs
else:
    import show_envs


def buildenvs():
    errors = []
    envs = show_envs.getenvs()

    if not envs:
        return ['No PlatformIO environments were found in ' + str(show_envs.PLATFORMIO_INI)]

    for index, env in enumerate(envs, start=1):
        print(f'[{index}/{len(envs)}] Building {env}...', flush=True)
        try:
            subprocess.run(
                ['pio', 'run', '-e', env],
                check=True,
                cwd=show_envs.PROJECT_ROOT,
            )
        except subprocess.CalledProcessError as cpe:
            errors.append('Process \'' + ' '.join(cpe.cmd) + '\' completed with error code ' + str(cpe.returncode))

    return errors

if __name__ == '__main__':
    errors = buildenvs()

    print()
    print('=' * 79)
    print()

    if len(errors) > 0:
        print('Builds completed with errors:')
        for error in errors:
            print('* ' + error)

        exit(1)

    else:
        print('Builds completed successfully.')

        exit()
