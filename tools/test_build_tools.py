#!/usr/bin/env python3

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import build_all, show_envs


class ShowEnvsTests(unittest.TestCase):
    def test_default_config_is_found_outside_project_root(self):
        previous_cwd = Path.cwd()
        with tempfile.TemporaryDirectory() as temporary_directory:
            try:
                os.chdir(temporary_directory)
                envs = show_envs.getenvs()
            finally:
                os.chdir(previous_cwd)

        self.assertIn('demo', envs)

    def test_missing_config_is_an_error(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            missing_config = Path(temporary_directory) / 'platformio.ini'
            with self.assertRaises(FileNotFoundError):
                show_envs.getenvs(missing_config)


class BuildAllTests(unittest.TestCase):
    @mock.patch('tools.build_all.subprocess.run')
    @mock.patch('tools.build_all.show_envs.getenvs', return_value=['demo', 'spectrum'])
    def test_builds_each_environment_from_project_root(self, getenvs, run):
        self.assertEqual([], build_all.buildenvs())

        getenvs.assert_called_once_with()
        self.assertEqual(
            [
                mock.call(
                    ['pio', 'run', '-e', 'demo'],
                    check=True,
                    cwd=show_envs.PROJECT_ROOT,
                ),
                mock.call(
                    ['pio', 'run', '-e', 'spectrum'],
                    check=True,
                    cwd=show_envs.PROJECT_ROOT,
                ),
            ],
            run.call_args_list,
        )

    @mock.patch('tools.build_all.subprocess.run')
    @mock.patch('tools.build_all.show_envs.getenvs', return_value=[])
    def test_empty_environment_list_is_an_error(self, getenvs, run):
        errors = build_all.buildenvs()

        self.assertEqual(1, len(errors))
        self.assertIn('No PlatformIO environments were found', errors[0])
        getenvs.assert_called_once_with()
        run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
