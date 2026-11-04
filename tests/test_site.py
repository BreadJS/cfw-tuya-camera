"""Verify the Pages artifact and links without vendor assets or network access."""
from html.parser import HTMLParser
from pathlib import Path
import sys
import tempfile
import unittest
from urllib.parse import unquote, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from build_site import build
from profiles import ROOT, load


class Document(HTMLParser):
    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.links = []
        self.ids = set()
        self.cards = {}
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if 'id' in attrs:
            self.ids.add(attrs['id'])
        for key in ('href', 'src'):
            if key in attrs:
                self.links.append(attrs[key])
        if 'data-camera' in attrs:
            self.cards[attrs['data-camera']] = attrs


class Site(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output = build('example/camera-firmware', Path(self.temp.name) / 'site')

    def test_pages_and_filter_data_match_profiles(self):
        document = Document((self.output / 'index.html').read_text())
        profiles = [load(path) for path in (ROOT / 'boards').glob('*.json')]
        self.assertEqual(set(document.cards), {p['board']['id'] for p in profiles})
        self.assertTrue({'search', 'chip', 'ptz-only', 'result-count', 'empty', 'clear-filters', 'toast'} <= document.ids)
        for profile in profiles:
            board = profile['board']
            card = document.cards[board['id']]
            self.assertEqual(card['data-chip'], profile['soc']['id'])
            self.assertEqual(card['data-ptz'], str(board['capabilities']['ptz']).lower())
            detail = (self.output / 'cameras' / (board['id'] + '.html')).read_text()
            self.assertIn(board['name'], detail)
            self.assertIn('https://github.com/example/camera-firmware/blob/HEAD/boards/', detail)

    def test_all_local_links_and_fragments_exist(self):
        for path in self.output.rglob('*.html'):
            for link in Document(path.read_text()).links:
                with self.subTest(page=path.name, link=link):
                    url = urlsplit(link)
                    if url.scheme or url.netloc:
                        self.assertEqual(url.scheme, 'https')
                        continue
                    self.assertFalse(url.path.startswith('/'), 'Links must work on project Pages URLs')
                    target = (path.parent / unquote(url.path)).resolve() if url.path else path
                    self.assertTrue(target.is_relative_to(self.output))
                    self.assertTrue(target.is_file())
                    if url.fragment:
                        self.assertIn(unquote(url.fragment), Document(target.read_text()).ids)

    def test_only_public_assets_are_published(self):
        self.assertTrue((self.output / '.nojekyll').is_file())
        for path in self.output.rglob('*'):
            if path.is_file():
                self.assertIn(path.suffix, {'.html', '.css', '.js', '.svg', '.webp', '.jpg', ''})
        self.assertFalse((self.output / 'local').exists())
        self.assertFalse((self.output / 'toolchains').exists())

    def test_invalid_repository_is_rejected(self):
        for repository in ('', 'https://github.com/a/b', 'a/b/c', '../b', 'a/..', 'a/b?x=1'):
            with self.subTest(repository=repository), self.assertRaises(ValueError):
                build(repository, self.output)


if __name__ == '__main__':
    unittest.main()
