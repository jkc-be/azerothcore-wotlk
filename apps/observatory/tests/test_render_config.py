import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('render_config', Path(__file__).parents[1] / 'render_config.py')
render_config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(render_config)

TEMPLATE = '''#
# Example section
#
#    BindIP
#        Default: "0.0.0.0"

BindIP = "0.0.0.0"
LoginDatabaseInfo     = "127.0.0.1;3306;acore;acore;acore_auth"
# Console.Enable = 1 is commented out here and must stay a comment
Console.Enable = 1
Rate.XP.Kill = 1
'''


class Rendering(unittest.TestCase):
    def test_overlays_replace_in_place_keep_comments_and_append_unknown_keys(self):
        example = render_config.parse('Console.Enable = 0\nObservatory.Enable = 1\nObservatory.BotCount = 100\n')
        local = render_config.parse('BindIP = "127.0.0.1"\nObservatory.BotCount = 30\n')
        secrets = render_config.parse('LoginDatabaseInfo = "127.0.0.1;3307;obs;pw;obs_auth"\n')
        text, appended = render_config.render(TEMPLATE, [example, local, secrets],
                                              [('Observatory.Directory', '"/runs/run-2"')])
        self.assertEqual(appended, ['Observatory.Enable', 'Observatory.BotCount', 'Observatory.Directory'])
        lines = text.splitlines()
        self.assertEqual(lines[:5], TEMPLATE.splitlines()[:5])  # comments and the commented default survive
        self.assertIn('# Console.Enable = 1 is commented out here and must stay a comment', lines)
        self.assertEqual(render_config.parse(text), {
            'BindIP': '"127.0.0.1"', 'LoginDatabaseInfo': '"127.0.0.1;3307;obs;pw;obs_auth"', 'Console.Enable': '0',
            'Rate.XP.Kill': '1', 'Observatory.Enable': '1', 'Observatory.BotCount': '30',
            'Observatory.Directory': '"/runs/run-2"'})
        self.assertLess(text.index('Rate.XP.Kill = 1'), text.index(render_config.APPENDED))
        self.assertEqual(text.count('Observatory.BotCount'), 1)  # the later overlay won, once

    def test_parse_takes_the_last_duplicate_and_ignores_comments(self):
        self.assertEqual(render_config.parse('# A = 1\nA = 1\n A =  2 \n'), {'A': '2'})

    def test_derive_reports_only_differences_from_the_layered_baseline(self):
        current = (TEMPLATE.replace('BindIP = "0.0.0.0"', 'BindIP = "127.0.0.1"')
                   + 'Observatory.Enable = 1\nRate.XP.Kill = 1\n')
        self.assertEqual(render_config.derive(TEMPLATE, current),
                         {'BindIP': '"127.0.0.1"', 'Observatory.Enable': '1'})
        example = render_config.parse('Observatory.Enable = 1\nRate.XP.Kill = 2\n')
        # Against template + example, the example's Rate.XP.Kill = 2 must be pinned back to 1 by the local overlay.
        self.assertEqual(render_config.derive(TEMPLATE, current, [example]),
                         {'BindIP': '"127.0.0.1"', 'Rate.XP.Kill': '1'})

    def test_cli_check_fails_on_differences_and_hides_secret_values(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'w.conf.dist').write_text(TEMPLATE)
            (root / 'secrets.conf').write_text('LoginDatabaseInfo = "127.0.0.1;3307;obs;pw;obs_auth"\n')
            (root / 'current.conf').write_text(TEMPLATE)
            import contextlib
            import io
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                status = render_config.main(['render', '--template', str(root / 'w.conf.dist'), '--overlay',
                                             str(root / 'secrets.conf'), '--check', str(root / 'current.conf'),
                                             '--output', str(root / 'out.conf')])
            self.assertEqual(status, 1)
            self.assertIn('LoginDatabaseInfo: current (hidden) -> rendered (hidden)', errors.getvalue())
            self.assertFalse((root / 'out.conf').exists())
            status = render_config.main(['render', '--template', str(root / 'w.conf.dist'), '--overlay',
                                         str(root / 'secrets.conf'), '--check', str(root / 'w.conf.dist'),
                                         '--set', 'LoginDatabaseInfo="127.0.0.1;3306;acore;acore;acore_auth"',
                                         '--output', str(root / 'out.conf')])
            self.assertEqual(status, 0)
            self.assertEqual(render_config.parse((root / 'out.conf').read_text()), render_config.parse(TEMPLATE))


if __name__ == '__main__':
    unittest.main()
