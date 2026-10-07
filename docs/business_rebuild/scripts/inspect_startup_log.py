"""Read-only, bounded historical log inspection; never imports sample modules."""
from pathlib import Path
import argparse
import json
import re


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('file')
    parser.add_argument('--first', type=int, default=1)
    parser.add_argument('--last', type=int, default=110)
    args = parser.parse_args()
    with Path(args.file).open('rb') as stream:
        offset = 0
        for number, raw in enumerate(stream, 1):
            if number > args.last:
                break
            start = offset
            offset += len(raw)
            if number < args.first:
                continue
            text = raw.decode('utf-8-sig', errors='replace').strip()
            match = re.match(r'.*? - (?:INFO|WARNING|ERROR|DEBUG) - (.*)', text)
            message = match[1] if match else text
            if '[{' in message and '"words"' in message:
                index = message.index('[{')
                try:
                    data = json.loads(message[index:])
                except ValueError:
                    continue
                words = [item.get('words', '') for item in data]
                words = [word for word in words if len(word) < 80 and not re.search(r'\d{12,}|UID|世界|战术联盟', word)]
                print(f'{number} @{start} ROI={message[:index]} WORDS=' + ' | '.join(words))
            elif '识别结果' not in message and len(message) < 500:
                print(f'{number} @{start} {message}')


if __name__ == '__main__':
    main()
