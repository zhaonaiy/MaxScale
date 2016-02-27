/**
 * Firewall filter syntax error test
 *
 * Generate various syntax errors and check if they are detected.
 */

#include <my_config.h>
#include <iostream>
#include <unistd.h>
#include "testconnections.h"
#include "config_check.h"
#include "fw_copy_rules.h"

const char *temp_rules = "rules_tmp.txt";

const char *rules_failure[] =
    {
        "rule testrule deny nothing",
        "rule testrule deny regex",
        "rule testrule deny columns",
        "rule testrule deny limit_queries",
        "rule testrule deny no-where-clause",
        "rule testrule deny wildcard wildcard",
        "rule testrule deny wildcard rule testrule deny no_where_clause",
        "rule testrule allow anything",
        "rule testrule block",
        "rule deny wildcard",
        "testrule deny wildcard",
        "rule testrule deny wildcard on_queries select | not_select",
        "rule testrule deny wildcard on_queries select|not_select",
        "rule testrule deny wildcard on_queries select |",
        "rule testrule deny wildcard on_queries select|",
        "rule ᐫᐬᐭᐮᐯᐰᐱ deny wildcard on_queries select|",
        NULL
    };

void add_rule(const char *rule)
{
    FILE *file = fopen(temp_rules, "a");
    fprintf(file, "%s\n", rule);
    fclose(file);
}

void copy_rule(TestConnections* test)
{
    char dest[PATH_MAX];
    sprintf(dest, "%s/rules/rules.txt", test->maxscale_access_homedir);
    test->copy_to_maxscale((char*)temp_rules, dest);
}

int main(int argc, char** argv)
{
    TestConnections *test = new TestConnections(argc, argv);
    test->stop_timeout();
    test->stop_maxscale();

    for (int i = 0; rules_failure[i]; i++)
    {
        test->set_timeout(40);
        add_rule(rules_failure[i]);
        copy_rules(test, (char*) "rules_tmp.txt", (char*) "");
        if (test_config_works("fwf_syntax"))
        {
            test->add_result(1, "Rule syntax error was not detected: %s\n", rules_failure[i]);
        }
    }
    test->check_maxscale_processes(0);
    test->copy_all_logs();  return test->global_result;
}
