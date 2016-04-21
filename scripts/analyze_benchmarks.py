'''
Copyright (c) 2008-2014, Pedigree Developers

Please see the CONTRIB file in the root of the source tree for a full
list of contributors.

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
'''

from __future__ import print_function

import argparse
import logging
import math
import sys
try:
    import simplejson as json
except ImportError:
    import json


def to_json(results):
    return json.dumps(results)


def to_text(results):
    result = ''
    for benchmark_name, benchmark_data in results.items():
        analysis = benchmark_data['analysis']
        row = benchmark_name + ': '
        row += ''
        row += 'changed from %(adjusted_prev)s %(human_unit)s '
        row += 'to %(adjusted_curr)s %(human_unit)s, which is '
        row += '%(human_diff)s %(human_unit)s '
        if math.isnan(analysis['factor_change']):
            row += '(an insignificant change)\n'
        else:
            row += '(%(percent_change)+.2f%% - '
            row += 'a %(factor_change).2fx change)\n'

        result += row % analysis

    return result


def load_benchmark_entry(entry):
    result = {}

    for k in ('items_per_second', 'bytes_per_second'):
        if k in entry:
            result[k] = entry[k]

    return result


def factor(a, b):
    return b / float(a)


def percent(a, b):
    return ((b - a) / float(b)) * 100.0


def adjust_human(value):
    if abs(value) < 1000.0:
        return '%.2f' % value
    elif abs(value) < 1000000.0:
        return '%.2fk' % (value / 1000.0)
    elif abs(value) < 1000000000.0:
        return '%.2fM' % (value / 1000000.0)
    elif abs(value) < 1000000000000.0:
        return '%.2fG' % (value / 1000000000.0)
    else:
        return '%.2fT' % (value / 1000000000000.0)


def analyze_results(prev_result, curr_result):
    metric_key = None
    metric_desc = None
    if 'items_per_second' in prev_result:
        metric_key = 'items_per_second'
        metric_desc = 'items/s'
    else:
        metric_key = 'bytes_per_second'
        metric_desc = 'B/s'

    prev_metric = prev_result.get(metric_key, 0.0)
    curr_metric = curr_result.get(metric_key, 0.0)

    fact = factor(prev_metric, curr_metric)
    if 1.0 - abs(fact) < 0.1:
        # Normalize as we really don't care about e.g. a 0.99x factor.
        # That kind of change is essentially just variance in the benchmarking
        # mechanism and not useful data.
        fact = float('nan')

    return {
        'diff': curr_metric - prev_metric,
        'percent_change': percent(prev_metric, curr_metric),
        'factor_change': fact,
        'human_diff': adjust_human(curr_metric - prev_metric),
        'human_unit': metric_desc,
        'adjusted_prev': adjust_human(prev_metric),
        'adjusted_curr': adjust_human(curr_metric),
    }


def main():
    logging.basicConfig(level=logging.INFO)

    parser = argparse.ArgumentParser(
        description='Pedigree benchmark analysis.')
    parser.add_argument('--prev', type=str, required=False,
                        help='path to previous benchmark results.')
    parser.add_argument('--current', type=str, required=True,
                        help='path to current benchmark results.')
    parser.add_argument('--format', type=str, required=True,
                        choices=('json', 'text'), default='text',
                        help='format to print the results in')
    parser.add_argument('--only_means', type=bool, default=True,
                        help='only compare the mean of multiple benchmark '
                             'repetitions (recommended)')
    args = parser.parse_args()

    # Load up the two input files to start with.
    if args.prev:
        with open(args.prev) as f:
            prev_results = json.load(f)
    with open(args.current) as f:
        current_results = json.load(f)

    # Collate into a results dictionary.
    benchmark_results = {}
    if args.prev:
        for entry in prev_results.get('benchmarks', ()):
            name = entry['name']
            if args.only_means and 'mean' not in name:
                continue
            else:
                name = name.strip('_mean')

            benchmark_results[name] = {
                'prev': load_benchmark_entry(entry),
            }

    for entry in current_results.get('benchmarks', ()):
        name = entry['name']
        if args.only_means and 'mean' not in name:
            continue
        else:
            name = name.strip('_mean')

        if name not in benchmark_results:
            print('Benchmark %r was newly introduced, ignoring.' % name)
            continue

        benchmark_results[name]['curr'] = load_benchmark_entry(entry)

        # Analyze the results here.
        if args.prev:
            analysis = analyze_results(benchmark_results[name]['prev'],
                                       benchmark_results[name]['curr'])
            benchmark_results[name]['analysis'] = analysis

    if args.format == 'json':
        print(to_json(benchmark_results))
    elif args.format == 'text':
        print(to_text(benchmark_results))


if __name__ == '__main__':
    main()
