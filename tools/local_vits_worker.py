"""Local inference worker. UTF-8 JSON lines over owned stdin/stdout; no network listener.

Runtime, dependencies and third party models are installed separately from Console.
Only text-to-speech runs here; this worker never receives model API credentials.
"""
import argparse
import base64
import contextlib
import importlib.util
import json
import math
import pathlib
import re
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', required=True)
    args = parser.parse_args()
    sys.stdin.reconfigure(encoding='utf-8')
    sys.stdout.reconfigure(encoding='utf-8')
    runtime = pathlib.Path(args.runtime).resolve()
    settings = json.loads((runtime / 'voice.json').read_text(encoding='utf-8'))
    sys.path.insert(0, str(runtime / 'vits'))
    with contextlib.redirect_stdout(sys.stderr):
        import numpy as np
        import torch
        from scipy.signal import resample_poly
        from models import SynthesizerTrn
        spec = importlib.util.spec_from_file_location('mandarin', runtime / 'vits/text/mandarin.py')
        mandarin = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mandarin)
        torch.set_num_threads(4)
        config = json.loads((runtime / settings['config']).read_text(encoding='utf-8'))
        # weights_only refuses arbitrary Python globals in public checkpoints.
        checkpoint = torch.load(runtime / settings['model'], map_location='cpu', weights_only=True)
        assert config['data']['text_cleaners'] == ['zh_ja_mixture_cleaners']
        assert len(config['symbols']) == checkpoint['model']['enc_p.emb.weight'].shape[0]
        speaker = settings['speaker_id']
        assert config['speakers'][speaker] == settings['speaker_name']
        model = SynthesizerTrn(len(config['symbols']), config['data']['filter_length'] // 2 + 1,
                              config['train']['segment_size'] // config['data']['hop_length'],
                              n_speakers=config['data']['n_speakers'], **config['model'])
        model.load_state_dict(checkpoint['model'], strict=True)
        model.eval()
        del checkpoint
    symbols = {symbol: i for i, symbol in enumerate(config['symbols'])}
    rate = config['data']['sampling_rate']
    divisor = math.gcd(rate, 16000)
    print(json.dumps({'event': 'ready'}), flush=True)
    for line in iter(lambda: sys.stdin.readline(4097), ''):
        identifier = ''
        try:
            if len(line) > 4096 or not line.endswith('\n'):
                raise ValueError('invalid_request')
            request = json.loads(line)
            identifier, text = request['id'], request['text']
            if not isinstance(identifier, str) or not re.fullmatch(r'[a-zA-Z0-9_-]{1,128}', identifier):
                raise ValueError('invalid_request')
            if not isinstance(text, str) or not text.strip() or len(text.encode('utf-8')) > 384:
                raise ValueError('invalid_text')
            stamp = time.monotonic()
            with contextlib.redirect_stdout(sys.stderr), torch.inference_mode():
                # Exact Chinese branch of the model's zh_ja_mixture_cleaners.
                cleaned = mandarin.chinese_to_romaji(text) + ' '
                cleaned = cleaned.rstrip()
                cleaned = re.sub(r'([^\.,!\?\-…~])$', r'\1.', cleaned)
                sequence = [symbols[c] for c in cleaned if c in symbols]
                if not sequence or len(sequence) > 1536:
                    raise ValueError('invalid_text')
                if config['data']['add_blank']:
                    padded = [0] * (len(sequence) * 2 + 1)
                    padded[1::2] = sequence
                    sequence = padded
                torch.manual_seed(1234)
                samples = model.infer(torch.LongTensor(sequence)[None, :], torch.LongTensor([len(sequence)]),
                                      sid=torch.LongTensor([speaker]), noise_scale=.667,
                                      noise_scale_w=.8, length_scale=1.0)[0][0, 0].numpy()
                if not np.isfinite(samples).all() or len(samples) > rate * 40:
                    raise ValueError('invalid_audio')
                samples = resample_poly(samples, 16000 // divisor, rate // divisor)
                peak = max(.001, float(np.max(np.abs(samples))))
                if peak > .9:
                    samples *= .9 / peak
                pcm = (np.clip(samples, -1, 1) * 32767).astype('<i2').tobytes()
            if not pcm or len(pcm) > 1280000:
                raise ValueError('invalid_audio')
            result = {'id': identifier, 'sample_rate': 16000, 'pcm': base64.b64encode(pcm).decode('ascii'),
                      'synthesis_ms': round((time.monotonic() - stamp) * 1000)}
        except Exception:
            # Never echo supplied text, environment, paths or third party tracebacks.
            result = {'id': identifier, 'error': 'local_synthesis_failed'}
        print(json.dumps(result, ensure_ascii=True, separators=(',', ':')), flush=True)


if __name__ == '__main__':
    main()
