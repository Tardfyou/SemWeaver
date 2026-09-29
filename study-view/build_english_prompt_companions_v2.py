"""Correctly attribute translation glossary preparation to the writing assistant."""
from pathlib import Path


if __name__ == '__main__':
    original = Path(__file__).with_name('build_english_prompt_companions.py')
    source = original.read_text()
    changes = {'Human-authored': 'Assistant-prepared', 'human-authored': 'assistant-prepared',
               'No model call': 'No experiment-model call',
               "'generator_sha256': sha(Path(__file__).read_bytes()),":
               "'generator_sha256': sha(Path(__file__).read_bytes()), 'input_generator_sha256': sha(original.read_bytes()),"}
    for old, new in changes.items():
        assert source.count(old) == 1
        source = source.replace(old, new)
    exec(compile(source, str(original)+'[accurate-translation-origin]', 'exec'),
         {'__name__': '__main__', '__file__': str(Path(__file__).resolve()), 'original': original})
