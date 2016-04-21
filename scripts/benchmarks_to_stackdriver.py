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

import datetime
import sys
import time
try:
    import simplejson as json
except ImportError:
    import json

from oauth2client.client import GoogleCredentials
from apiclient import discovery


def create_metrics(client):
    project_id = 'the-pedigree-project'
    metric_type = 'custom.googleapis.com/items_per_second'
    metrics_descriptor = {
        'name': 'projects/{}/metricDescriptors/{}'.format(
            project_id, metric_type),
        'type': metric_type,
        'labels': [
            {
                'key': 'benchmark',
                'valueType': 'STRING',
                'description': 'Name of the benchmark',
            },
        ],
        'metricKind': 'GAUGE',
        'valueType': 'INT64',
        'unit': 'items/s',
        'description': 'Items/s processed during the benchmark.',
        'displayName': 'Items/s Processed'
    }

    client.projects().metricDescriptors().create(
        name='projects/' + project_id, body=metrics_descriptor).execute()

    metric_type = 'custom.googleapis.com/bytes_per_second'
    metrics_descriptor = {
        'name': 'projects/{}/metricDescriptors/{}'.format(
            project_id, metric_type),
        'type': metric_type,
        'labels': [
            {
                'key': 'benchmark',
                'valueType': 'STRING',
                'description': 'Name of the benchmark',
            },
        ],
        'metricKind': 'GAUGE',
        'valueType': 'INT64',
        'unit': 'bytes/s',
        'description': 'Bytes/s processed during the benchmark.',
        'displayName': 'Bytes/s Processed'
    }

    client.projects().metricDescriptors().create(
        name='projects/' + project_id, body=metrics_descriptor).execute()


def get_custom_metric(client, name):
    project_id = 'projects/the-pedigree-project'
    request = client.projects().metricDescriptors().list(
        name=project_id,
        filter='metric.type=starts_with("custom.googleapis.com/%s")' % name)

    response = request.execute()
    try:
        return response['metricDescriptors']
    except KeyError:
        return None


def wait_for_metrics(client):
    print('waiting for metrics to be created')
    while True:
        a = get_custom_metric(client, 'items_per_second')
        b = get_custom_metric(client, 'bytes_per_second')

        if a and b:
            break

        time.sleep(1.0)
    print('done waiting')


def write_value(client, key, benchmark, value):
    # TODO(miselin): attach the time of the commit instead.
    project_id = 'projects/the-pedigree-project'
    now = datetime.datetime.utcnow().isoformat('T') + 'Z'
    timeseries_data = {
        'metric': {
            'type': 'custom.googleapis.com/' + key,
            'labels': {
                'benchmark': benchmark,
            },
        },
        'resource': {
            'type': 'gce_instance',
            'labels': {
                'instance_id': 'benchmarker-vm',
                'zone': 'us-central1-b',
            },
        },
        'metricKind': 'GAUGE',
        'valueType': 'INT64',
        'points': [
            {
                'interval': {
                    'startTime': now,
                    'endTime': now,
                },
                'value': {
                    'int64Value': value,
                },
            },
        ]
    }

    request = client.projects().timeSeries().create(
        name=project_id, body={'timeSeries': [timeseries_data]})
    request.execute()


def main():
    print('hallo, uploading')
    return

    # Reads json from stdin, turns it into metrics and pushes into Stackdriver
    # monitoring for further analysis.
    credentials = GoogleCredentials.get_application_default()
    client = discovery.build('monitoring', 'v3', credentials=credentials)

    # Make sure we have the metric definition available.
    create_metrics(client)
    wait_for_metrics(client)

    input_data = json.load(sys.stdin)

    for benchmark_name, benchmark_data in input_data.items():
        current = benchmark_data['curr']

        name = benchmark_name

        items_per_second = current.get('items_per_second')
        bytes_per_second = current.get('bytes_per_second')

        if items_per_second:
            write_value(client, 'items_per_second', name, items_per_second)
        elif bytes_per_second:
            write_value(client, 'bytes_per_second', name, bytes_per_second)


if __name__ == '__main__':
    main()
