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


import webapp2


class BenchmarkView(webapp2.RequestHandler):

    def doPurge(self):
        """Purge long-running jobs and update their commit status."""
        pass

    def doCommit(self):
        """Incoming webhook from github - adds a new benchmark job."""
        # todo: check branch, if develop then we can kick off an extra job to
        # do the necessary single-branch monitoring statistics update
        pass

    def doCommitDone(self):
        """Incoming success status from a benchmark that's finished."""
        pass

    def doShowDetails(self):
        """Shows details of all past benchmark jobs."""
        pass

    def get(self):
        what = self.request.get('what')
        if what == 'purge':
            self.doPurge()
        elif what == 'commit':
            self.doCommit()
        elif what == 'done':
            self.doCommitDone()
        else:
            self.doShowDetails()

        self.response.headers['Content-Type'] = 'text/plain'
        self.response.write('Hello, world!')
