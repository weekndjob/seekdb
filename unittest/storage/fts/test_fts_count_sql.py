#!/usr/bin/env python3
"""Exercise score-free COUNT retrieval against a running disposable seekdb instance."""
import argparse
import os
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', required=True)
    parser.add_argument('--socket', required=True)
    args = parser.parse_args()
    client = [args.client, '--no-defaults', '--socket=' + args.socket,
              '-uroot', '-B', '-N', '--raw', '--default-character-set=utf8mb4']
    database = 'fts_count_regression_%d_%d' % (os.getpid(), time.time_ns())

    def sql(statement):
        result = subprocess.run(client, input=statement, text=True,
                                capture_output=True, timeout=30)
        if result.returncode:
            raise AssertionError(statement + '\n' + result.stderr)
        return result.stdout.strip().splitlines()

    def expect(statement, expected):
        actual = sql('USE ' + database + ';' + statement)
        assert actual == expected, (statement, expected, actual)

    sql('CREATE DATABASE ' + database)
    try:
        sql('USE ' + database + '; CREATE TABLE docs ('
            'id INT PRIMARY KEY, body TEXT, FULLTEXT INDEX ft(body) WITH PARSER space);'
            "INSERT INTO docs VALUES (1,'alpha beta'),(2,'alpha alpha'),"
            "(3,'beta gamma'),(4,NULL),(5,'');")
        match = "MATCH(body) AGAINST('alpha')"
        expect('SELECT COUNT(*) FROM docs WHERE ' + match, ['2'])
        expect("SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST('alpha beta')", ['3'])
        for term in ['', 'zznotfound98765']:
            expect("SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST('%s')" % term, ['0'])
        expect('SELECT COUNT(*) FROM docs WHERE ' + match + '>0', ['2'])
        expect('SELECT COUNT(*) FROM docs WHERE ' + match + '>1000000', ['0'])
        expect('SELECT COUNT(*) FROM docs WHERE ' + match + ' AND id=2', ['1'])
        expect('SELECT COUNT(*) FROM docs WHERE ' + match + ' OR id=3', ['3'])
        expect('SELECT COUNT(*) FROM docs WHERE ' + match + ' HAVING COUNT(*)>1', ['2'])
        expect('SELECT COUNT(*) FROM (SELECT id FROM docs WHERE ' + match + ' LIMIT 1) t', ['1'])
        expect("SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST('+alpha -beta' IN BOOLEAN MODE)", ['1'])
        expect("PREPARE s FROM 'SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(?)';"
               "SET @q='alpha'; EXECUTE s USING @q; SET @q='gamma'; EXECUTE s USING @q;"
               'DEALLOCATE PREPARE s', ['2', '1'])
        expect('SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(NULL)', ['0'])
        # The projected score and a numeric threshold must still use real BM25 scores.
        plans = [
            ('SELECT COUNT(*) FROM docs WHERE ' + match, 'calc_relevance=false'),
            ('SELECT COUNT(*) FROM docs WHERE ' + match + '>0', None),
            ('SELECT ' + match + ' FROM docs WHERE ' + match, 'calc_relevance=true'),
        ]
        for query, marker in plans:
            plan = '\n'.join(sql('USE ' + database + ';EXPLAIN ' + query))
            if marker is None:
                assert 'calc_relevance=false' not in plan, (query, plan)
            else:
                assert marker in plan, (query, marker, plan)
        expect('DELETE FROM docs WHERE id=2;SELECT COUNT(*) FROM docs WHERE ' + match, ['1'])
        expect("INSERT INTO docs VALUES(6,'alpha delta');SELECT COUNT(*) FROM docs WHERE " + match, ['2'])
        terms = ' '.join('token%d' % i for i in range(300))
        expect("INSERT INTO docs VALUES(7,'%s');SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST('%s')" % (terms, terms), ['1'])
        expect("PREPARE s FROM 'SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST(?)';"
               "SET @q='%s'; EXECUTE s USING @q; SET @q='alpha'; EXECUTE s USING @q;"
               'DEALLOCATE PREPARE s' % terms, ['1', '2'])
        print('PASS: COUNT membership, thresholds, ranking, boolean mode, prepared statements, NULL, DML, 300-term retrieval and plans')
    finally:
        sql('DROP DATABASE ' + database)


if __name__ == '__main__':
    main()
