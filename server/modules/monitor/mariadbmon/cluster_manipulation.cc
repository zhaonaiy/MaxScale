/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "mariadbmon.hh"

#include <inttypes.h>
#include <sstream>
#include <maxscale/hk_heartbeat.h>
#include <maxscale/mysql_utils.h>
#include "utilities.hh"

bool MariaDBMonitor::manual_switchover(MXS_MONITORED_SERVER* new_master,
                                       MXS_MONITORED_SERVER* given_current_master,
                                       json_t** error_out)
{
    bool stopped = stop();
    if (stopped)
    {
        MXS_NOTICE("Stopped the monitor %s for the duration of switchover.", m_monitor_base->name);
    }
    else
    {
        MXS_NOTICE("Monitor %s already stopped, switchover can proceed.", m_monitor_base->name);
    }

    // Autoselect current master if not given as parameter.
    MXS_MONITORED_SERVER* current_master = given_current_master;
    if (current_master == NULL)
    {
        if (m_master)
        {
            current_master = m_master;
        }
        else
        {
            const char NO_MASTER[] = "Monitor '%s' has no master server.";
            PRINT_MXS_JSON_ERROR(error_out, NO_MASTER, m_monitor_base->name);
        }
    }

    bool current_ok = (current_master != NULL) ? switchover_check_current(current_master, error_out) : false;
    bool new_ok = switchover_check_new(new_master, error_out);
    // Check that all slaves are using gtid-replication
    bool gtid_ok = true;
    for (auto mon_serv = m_monitor_base->monitored_servers; mon_serv != NULL; mon_serv = mon_serv->next)
    {
        if (SERVER_IS_SLAVE(mon_serv->server))
        {
            if (!uses_gtid(mon_serv, error_out))
            {
                gtid_ok = false;
            }
        }
    }

    bool rval = false;
    if (current_ok && new_ok && gtid_ok)
    {
        bool switched = do_switchover(current_master, new_master, error_out);

        const char* curr_master_name = current_master->server->unique_name;
        const char* new_master_name = new_master->server->unique_name;

        if (switched)
        {
            MXS_NOTICE("Switchover %s -> %s performed.", curr_master_name, new_master_name);
            rval = true;
        }
        else
        {
            string format = "Switchover %s -> %s failed";
            bool failover_setting = config_get_bool(m_monitor_base->parameters, CN_AUTO_FAILOVER);
            if (failover_setting)
            {
                disable_setting(CN_AUTO_FAILOVER);
                format += ", automatic failover has been disabled.";
            }
            format += ".";
            PRINT_MXS_JSON_ERROR(error_out, format.c_str(), curr_master_name, new_master_name);
        }
    }

    if (stopped)
    {
        MariaDBMonitor::start(m_monitor_base, m_monitor_base->parameters);
    }
    return rval;
}

bool MariaDBMonitor::manual_failover(json_t** output)
{
    bool stopped = stop();
    if (stopped)
    {
        MXS_NOTICE("Stopped monitor %s for the duration of failover.", m_monitor_base->name);
    }
    else
    {
        MXS_NOTICE("Monitor %s already stopped, failover can proceed.", m_monitor_base->name);
    }

    bool rv = true;
    rv = failover_check(output);
    if (rv)
    {
        rv = do_failover(output);
        if (rv)
        {
            MXS_NOTICE("Failover performed.");
        }
        else
        {
            PRINT_MXS_JSON_ERROR(output, "Failover failed.");
        }
    }

    if (stopped)
    {
        MariaDBMonitor::start(m_monitor_base, m_monitor_base->parameters);
    }
    return rv;
}

bool MariaDBMonitor::manual_rejoin(SERVER* rejoin_server, json_t** output)
{
    bool stopped = stop();
    if (stopped)
    {
        MXS_NOTICE("Stopped monitor %s for the duration of rejoin.", m_monitor_base->name);
    }
    else
    {
        MXS_NOTICE("Monitor %s already stopped, rejoin can proceed.", m_monitor_base->name);
    }

    bool rval = false;
    if (cluster_can_be_joined())
    {
        const char* rejoin_serv_name = rejoin_server->unique_name;
        MXS_MONITORED_SERVER* mon_server = mon_get_monitored_server(m_monitor_base, rejoin_server);
        if (mon_server)
        {
            const char* master_name = m_master->server->unique_name;
            MariaDBServer* master_info = get_server_info(m_master);
            MariaDBServer* server_info = get_server_info(mon_server);

            if (server_is_rejoin_suspect(mon_server, master_info, output))
            {
                if (update_gtids(master_info))
                {
                    if (can_replicate_from(mon_server, server_info, master_info))
                    {
                        ServerVector joinable_server;
                        joinable_server.push_back(mon_server);
                        if (do_rejoin(joinable_server) == 1)
                        {
                            rval = true;
                            MXS_NOTICE("Rejoin performed.");
                        }
                        else
                        {
                            PRINT_MXS_JSON_ERROR(output, "Rejoin attempted but failed.");
                        }
                    }
                    else
                    {
                        PRINT_MXS_JSON_ERROR(output, "Server '%s' cannot replicate from cluster master '%s' "
                                             "or it could not be queried.", rejoin_serv_name, master_name);
                    }
                }
                else
                {
                    PRINT_MXS_JSON_ERROR(output, "Cluster master '%s' gtid info could not be updated.",
                                         master_name);
                }
            }
        }
        else
        {
            PRINT_MXS_JSON_ERROR(output, "The given server '%s' is not monitored by this monitor.",
                                 rejoin_serv_name);
        }
    }
    else
    {
        const char BAD_CLUSTER[] = "The server cluster of monitor '%s' is not in a state valid for joining. "
                                   "Either it has no master or its gtid domain is unknown.";
        PRINT_MXS_JSON_ERROR(output, BAD_CLUSTER, m_monitor_base->name);
    }

    if (stopped)
    {
        MariaDBMonitor::start(m_monitor_base, m_monitor_base->parameters);
    }
    return rval;
}

/**
 * Generate a CHANGE MASTER TO-query.
 *
 * @param master_host Master hostname/address
 * @param master_port Master port
 * @return Generated query
 */
string MariaDBMonitor::generate_change_master_cmd(const string& master_host, int master_port)
{
    std::stringstream change_cmd;
    change_cmd << "CHANGE MASTER TO MASTER_HOST = '" << master_host << "', ";
    change_cmd << "MASTER_PORT = " <<  master_port << ", ";
    change_cmd << "MASTER_USE_GTID = current_pos, ";
    change_cmd << "MASTER_USER = '" << m_replication_user << "', ";
    const char MASTER_PW[] = "MASTER_PASSWORD = '";
    const char END[] = "';";
#if defined(SS_DEBUG)
    std::stringstream change_cmd_nopw;
    change_cmd_nopw << change_cmd.str();
    change_cmd_nopw << MASTER_PW << "******" << END;;
    MXS_DEBUG("Change master command is '%s'.", change_cmd_nopw.str().c_str());
#endif
    change_cmd << MASTER_PW << m_replication_password << END;
    return change_cmd.str();
}

/**
 * Redirects slaves to replicate from another master server.
 *
 * @param slaves An array of slaves
 * @param new_master The replication master
 * @param redirected_slaves A vector where to insert successfully redirected slaves.
 * @return The number of slaves successfully redirected.
 */
int MariaDBMonitor::redirect_slaves(MXS_MONITORED_SERVER* new_master, const ServerVector& slaves,
                                    ServerVector* redirected_slaves)
{
    ss_dassert(redirected_slaves != NULL);
    MXS_NOTICE("Redirecting slaves to new master.");
    string change_cmd = generate_change_master_cmd(new_master->server->name, new_master->server->port);
    int successes = 0;
    for (ServerVector::const_iterator iter = slaves.begin(); iter != slaves.end(); iter++)
    {
        if (redirect_one_slave(*iter, change_cmd.c_str()))
        {
            successes++;
            redirected_slaves->push_back(*iter);
        }
    }
    return successes;
}

/**
 * Set the new master to replicate from the cluster external master.
 *
 * @param new_master The server being promoted
 * @param err_out Error output
 * @return True if new master accepted commands
 */
bool MariaDBMonitor::start_external_replication(MXS_MONITORED_SERVER* new_master, json_t** err_out)
{
    bool rval = false;
    string change_cmd = generate_change_master_cmd(m_external_master_host, m_external_master_port);
    if (mxs_mysql_query(new_master->con, change_cmd.c_str()) == 0 &&
        mxs_mysql_query(new_master->con, "START SLAVE;") == 0)
    {
        MXS_NOTICE("New master starting replication from external master %s:%d.",
                   m_external_master_host.c_str(), m_external_master_port);
        rval = true;
    }
    else
    {
        PRINT_MXS_JSON_ERROR(err_out, "Could not start replication from external master: '%s'.",
                             mysql_error(new_master->con));
    }
    return rval;
}

/**
 * Starts a new slave connection on a server. Should be used on a demoted master server.
 *
 * @param old_master The server which will start replication
 * @param new_master Replication target
 * @return True if commands were accepted. This does not guarantee that replication proceeds
 * successfully.
 */
bool MariaDBMonitor::switchover_start_slave(MXS_MONITORED_SERVER* old_master, SERVER* new_master)
{
    bool rval = false;
    string change_cmd = generate_change_master_cmd(new_master->name, new_master->port);
    if (mxs_mysql_query(old_master->con, change_cmd.c_str()) == 0 &&
        mxs_mysql_query(old_master->con, "START SLAVE;") == 0)
    {
        MXS_NOTICE("Old master '%s' starting replication from '%s'.",
                   old_master->server->unique_name, new_master->unique_name);
        rval = true;
    }
    else
    {
        MXS_ERROR("Old master '%s' could not start replication: '%s'.",
                  old_master->server->unique_name, mysql_error(old_master->con));
    }
    return rval;
}

/**
 * Redirect one slave server to another master
 *
 * @param slave Server to redirect
 * @param change_cmd Change master command, usually generated by generate_change_master_cmd()
 * @return True if slave accepted all commands
 */
bool MariaDBMonitor::redirect_one_slave(MXS_MONITORED_SERVER* slave, const char* change_cmd)
{
    bool rval = false;
    if (mxs_mysql_query(slave->con, "STOP SLAVE;") == 0 &&
        mxs_mysql_query(slave->con, "RESET SLAVE;") == 0 && // To erase any old I/O or SQL errors
        mxs_mysql_query(slave->con, change_cmd) == 0 &&
        mxs_mysql_query(slave->con, "START SLAVE;") == 0)
    {
        rval = true;
        MXS_NOTICE("Slave '%s' redirected to new master.", slave->server->unique_name);
    }
    else
    {
        MXS_WARNING("Slave '%s' redirection failed: '%s'.", slave->server->unique_name,
                    mysql_error(slave->con));
    }
    return rval;
}

/**
 * (Re)join given servers to the cluster. The servers in the array are assumed to be joinable.
 * Usually the list is created by get_joinable_servers().
 *
 * @param joinable_servers Which servers to rejoin
 * @return The number of servers successfully rejoined
 */
uint32_t MariaDBMonitor::do_rejoin(const ServerVector& joinable_servers)
{
    SERVER* master_server = m_master->server;
    uint32_t servers_joined = 0;
    if (!joinable_servers.empty())
    {
        string change_cmd = generate_change_master_cmd(master_server->name, master_server->port);
        for (ServerVector::const_iterator iter = joinable_servers.begin();
             iter != joinable_servers.end();
             iter++)
        {
            MXS_MONITORED_SERVER* joinable = *iter;
            const char* name = joinable->server->unique_name;
            const char* master_name = master_server->unique_name;
            MariaDBServer* redir_info = get_server_info(joinable);

            bool op_success;
            if (redir_info->n_slaves_configured == 0)
            {
                MXS_NOTICE("Directing standalone server '%s' to replicate from '%s'.", name, master_name);
                op_success = join_cluster(joinable, change_cmd.c_str());
            }
            else
            {
                MXS_NOTICE("Server '%s' is replicating from a server other than '%s', "
                           "redirecting it to '%s'.", name, master_name, master_name);
                op_success = redirect_one_slave(joinable, change_cmd.c_str());
            }

            if (op_success)
            {
                servers_joined++;
            }
        }
    }
    return servers_joined;
}

/**
 * Check if the cluster is a valid rejoin target.
 *
 * @return True if master and gtid domain are known
 */
bool MariaDBMonitor::cluster_can_be_joined()
{
    return (m_master != NULL && SERVER_IS_MASTER(m_master->server) && m_master_gtid_domain >= 0);
}

/**
 * Scan the servers in the cluster and add (re)joinable servers to an array.
 *
 * @param mon Cluster monitor
 * @param output Array to save results to. Each element is a valid (re)joinable server according
 * to latest data.
 * @return False, if there were possible rejoinable servers but communications error to master server
 * prevented final checks.
 */
bool MariaDBMonitor::get_joinable_servers(ServerVector* output)
{
    ss_dassert(output);
    MariaDBServer *master_info = get_server_info(m_master);

    // Whether a join operation should be attempted or not depends on several criteria. Start with the ones
    // easiest to test. Go though all slaves and construct a preliminary list.
    ServerVector suspects;
    for (MXS_MONITORED_SERVER* server = m_monitor_base->monitored_servers;
         server != NULL;
         server = server->next)
    {
        if (server_is_rejoin_suspect(server, master_info, NULL))
        {
            suspects.push_back(server);
        }
    }

    // Update Gtid of master for better info.
    bool comm_ok = true;
    if (!suspects.empty())
    {
        if (update_gtids(master_info))
        {
            for (size_t i = 0; i < suspects.size(); i++)
            {
                MXS_MONITORED_SERVER* suspect = suspects[i];
                MariaDBServer* suspect_info = get_server_info(suspect);
                if (can_replicate_from(suspect, suspect_info, master_info))
                {
                    output->push_back(suspect);
                }
            }
        }
        else
        {
            comm_ok = false;
        }
    }
    return comm_ok;
}

/**
 * Joins a standalone server to the cluster.
 *
 * @param server Server to join
 * @param change_cmd Change master command
 * @return True if commands were accepted by server
 */
bool MariaDBMonitor::join_cluster(MXS_MONITORED_SERVER* server, const char* change_cmd)
{
    /* Server does not have slave connections. This operation can fail, or the resulting
     * replication may end up broken. */
    bool rval = false;
    if (mxs_mysql_query(server->con, "SET GLOBAL read_only=1;") == 0 &&
        mxs_mysql_query(server->con, change_cmd) == 0 &&
        mxs_mysql_query(server->con, "START SLAVE;") == 0)
    {
        rval = true;
    }
    else
    {
        mxs_mysql_query(server->con, "SET GLOBAL read_only=0;");
    }
    return rval;
}

/**
 * Checks if a server is a possible rejoin candidate. A true result from this function is not yet sufficient
 * criteria and another call to can_replicate_from() should be made.
 *
 * @param server Server to check
 * @param master_info Master server info
 * @param output Error output. If NULL, no error is printed to log.
 * @return True, if server is a rejoin suspect.
 */
bool MariaDBMonitor::server_is_rejoin_suspect(MXS_MONITORED_SERVER* rejoin_server,
                                              MariaDBServer* master_info, json_t** output)
{
    bool is_suspect = false;
    if (!SERVER_IS_MASTER(rejoin_server->server) && SERVER_IS_RUNNING(rejoin_server->server))
    {
        MariaDBServer* server_info = get_server_info(rejoin_server);
        SlaveStatusInfo* slave_status = &server_info->slave_status;
        // Has no slave connection, yet is not a master.
        if (server_info->n_slaves_configured == 0)
        {
            is_suspect = true;
        }
        // Or has existing slave connection ...
        else if (server_info->n_slaves_configured == 1)
        {
            // which is connected to master but it's the wrong one
            if (slave_status->slave_io_running  &&
                slave_status->master_server_id != master_info->server_id)
            {
                is_suspect = true;
            }
            // or is disconnected but master host or port is wrong.
            else if (!slave_status->slave_io_running && slave_status->slave_sql_running &&
                     (slave_status->master_host != m_master->server->name ||
                      slave_status->master_port != m_master->server->port))
            {
                is_suspect = true;
            }
        }
    }
    else if (output != NULL)
    {
        PRINT_MXS_JSON_ERROR(output, "Server '%s' is master or not running.",
                             rejoin_server->server->unique_name);
    }
    return is_suspect;
}

/**
 * Performs switchover for a simple topology (1 master, N slaves, no intermediate masters). If an
 * intermediate step fails, the cluster may be left without a master.
 *
 * @param err_out json object for error printing. Can be NULL.
 * @return True if successful. If false, the cluster can be in various situations depending on which step
 * failed. In practice, manual intervention is usually required on failure.
 */
bool MariaDBMonitor::do_switchover(MXS_MONITORED_SERVER* current_master, MXS_MONITORED_SERVER* new_master,
                                   json_t** err_out)
{
    MXS_MONITORED_SERVER* demotion_target = current_master ? current_master : m_master;
    if (demotion_target == NULL)
    {
        PRINT_MXS_JSON_ERROR(err_out, "Cluster does not have a running master. Run failover instead.");
        return false;
    }
    if (m_master_gtid_domain < 0)
    {
        PRINT_MXS_JSON_ERROR(err_out, "Cluster gtid domain is unknown. Cannot switchover.");
        return false;
    }
    // Total time limit on how long this operation may take. Checked and modified after significant steps are
    // completed.
    int seconds_remaining = m_switchover_timeout;
    time_t start_time = time(NULL);
    // Step 1: Select promotion candidate, save all slaves except promotion target to an array. If we have a
    // user-defined master candidate, check it. Otherwise, autoselect.
    MXS_MONITORED_SERVER* promotion_target = NULL;
    ServerVector redirectable_slaves;
    if (new_master)
    {
        if (switchover_check_preferred_master(new_master, err_out))
        {
            promotion_target = new_master;
            /* User-given candidate is good. Update info on all slave servers.
             * The update_slave_info()-call is not strictly necessary here, but it should be ran to keep this
             * path analogous with failover_select_new_master(). The later functions can then assume that
             * slave server info is up to date.
             */
            for (MXS_MONITORED_SERVER* slave = m_monitor_base->monitored_servers; slave; slave = slave->next)
            {
                if (slave != promotion_target)
                {
                    MariaDBServer* slave_info = update_slave_info(slave);
                    // If master is replicating from external master, it is updated but not added to array.
                    if (slave_info && slave != current_master)
                    {
                        redirectable_slaves.push_back(slave);
                    }
                }
            }
        }
    }
    else
    {
        promotion_target = select_new_master(&redirectable_slaves, err_out);
    }
    if (promotion_target == NULL)
    {
        return false;
    }

    bool rval = false;
    MariaDBServer* curr_master_info = get_server_info(demotion_target);

    // Step 2: Set read-only to on, flush logs, update master gtid:s
    if (switchover_demote_master(demotion_target, curr_master_info, err_out))
    {
        bool catchup_and_promote_success = false;
        time_t step2_time = time(NULL);
        seconds_remaining -= difftime(step2_time, start_time);

        // Step 3: Wait for the slaves (including promotion target) to catch up with master.
        ServerVector catchup_slaves = redirectable_slaves;
        catchup_slaves.push_back(promotion_target);
        if (switchover_wait_slaves_catchup(catchup_slaves, curr_master_info->gtid_binlog_pos,
                                           seconds_remaining, m_monitor_base->read_timeout, err_out))
        {
            time_t step3_time = time(NULL);
            int seconds_step3 = difftime(step3_time, step2_time);
            MXS_DEBUG("Switchover: slave catchup took %d seconds.", seconds_step3);
            seconds_remaining -= seconds_step3;

            // Step 4: On new master STOP and RESET SLAVE, set read-only to off.
            if (promote_new_master(promotion_target, err_out))
            {
                catchup_and_promote_success = true;
                // Step 5: Redirect slaves and start replication on old master.
                ServerVector redirected_slaves;
                bool start_ok = switchover_start_slave(demotion_target, promotion_target->server);
                if (start_ok)
                {
                    redirected_slaves.push_back(demotion_target);
                }
                int redirects = redirect_slaves(promotion_target, redirectable_slaves,
                                                     &redirected_slaves);

                bool success = redirectable_slaves.empty() ? start_ok : start_ok || redirects > 0;
                if (success)
                {
                    time_t step5_time = time(NULL);
                    seconds_remaining -= difftime(step5_time, step3_time);

                    // Step 6: Finally, add an event to the new master to advance gtid and wait for the slaves
                    // to receive it. If using external replication, skip this step. Come up with an
                    // alternative later.
                    if (m_external_master_port != PORT_UNKNOWN)
                    {
                        MXS_WARNING("Replicating from external master, skipping final check.");
                        rval = true;
                    }
                    else if (wait_cluster_stabilization(promotion_target, redirected_slaves,
                                                        seconds_remaining))
                    {
                        rval = true;
                        time_t step6_time = time(NULL);
                        int seconds_step6 = difftime(step6_time, step5_time);
                        seconds_remaining -= seconds_step6;
                        MXS_DEBUG("Switchover: slave replication confirmation took %d seconds with "
                                  "%d seconds to spare.", seconds_step6, seconds_remaining);
                    }
                }
                else
                {
                    print_redirect_errors(demotion_target, redirectable_slaves, err_out);
                }
            }
        }

        if (!catchup_and_promote_success)
        {
            // Step 3 or 4 failed, try to undo step 2.
            const char QUERY_UNDO[] = "SET GLOBAL read_only=0;";
            if (mxs_mysql_query(demotion_target->con, QUERY_UNDO) == 0)
            {
                PRINT_MXS_JSON_ERROR(err_out, "read_only disabled on server %s.",
                                     demotion_target->server->unique_name);
            }
            else
            {
                PRINT_MXS_JSON_ERROR(err_out, "Could not disable read_only on server %s: '%s'.",
                                     demotion_target->server->unique_name, mysql_error(demotion_target->con));
            }

            // Try to reactivate external replication if any.
            if (m_external_master_port != PORT_UNKNOWN)
            {
                start_external_replication(new_master, err_out);
            }
        }
    }
    return rval;
}

/**
 * Performs failover for a simple topology (1 master, N slaves, no intermediate masters).
 *
 * @param err_out Json output
 * @return True if successful
 */
bool MariaDBMonitor::do_failover(json_t** err_out)
{
    // Topology has already been tested to be simple.
    if (m_master_gtid_domain < 0)
    {
        PRINT_MXS_JSON_ERROR(err_out, "Cluster gtid domain is unknown. Cannot failover.");
        return false;
    }
    // Total time limit on how long this operation may take. Checked and modified after significant steps are
    // completed.
    int seconds_remaining = m_failover_timeout;
    time_t start_time = time(NULL);
    // Step 1: Select new master. Also populate a vector with all slaves not the selected master.
    ServerVector redirectable_slaves;
    MXS_MONITORED_SERVER* new_master = select_new_master(&redirectable_slaves, err_out);
    if (new_master == NULL)
    {
        return false;
    }
    time_t step1_time = time(NULL);
    seconds_remaining -= difftime(step1_time, start_time);

    bool rval = false;
    // Step 2: Wait until relay log consumed.
    if (failover_wait_relay_log(new_master, seconds_remaining, err_out))
    {
        time_t step2_time = time(NULL);
        int seconds_step2 = difftime(step2_time, step1_time);
        MXS_DEBUG("Failover: relay log processing took %d seconds.", seconds_step2);
        seconds_remaining -= seconds_step2;

        // Step 3: Stop and reset slave, set read-only to 0.
        if (promote_new_master(new_master, err_out))
        {
            // Step 4: Redirect slaves.
            ServerVector redirected_slaves;
            int redirects = redirect_slaves(new_master, redirectable_slaves, &redirected_slaves);
            bool success = redirectable_slaves.empty() ? true : redirects > 0;
            if (success)
            {
                time_t step4_time = time(NULL);
                seconds_remaining -= difftime(step4_time, step2_time);

                // Step 5: Finally, add an event to the new master to advance gtid and wait for the slaves
                // to receive it. seconds_remaining can be 0 or less at this point. Even in such a case
                // wait_cluster_stabilization() may succeed if replication is fast enough. If using external
                // replication, skip this step. Come up with an alternative later.
                if (m_external_master_port != PORT_UNKNOWN)
                {
                    MXS_WARNING("Replicating from external master, skipping final check.");
                    rval = true;
                }
                else if (redirected_slaves.empty())
                {
                    // No slaves to check. Assume success.
                    rval = true;
                    MXS_DEBUG("Failover: no slaves to redirect, skipping stabilization check.");
                }
                else if (wait_cluster_stabilization(new_master, redirected_slaves, seconds_remaining))
                {
                    rval = true;
                    time_t step5_time = time(NULL);
                    int seconds_step5 = difftime(step5_time, step4_time);
                    seconds_remaining -= seconds_step5;
                    MXS_DEBUG("Failover: slave replication confirmation took %d seconds with "
                              "%d seconds to spare.", seconds_step5, seconds_remaining);
                }
            }
            else
            {
                print_redirect_errors(NULL, redirectable_slaves, err_out);
            }
        }
    }

    return rval;
}

/**
 * Waits until the new master has processed all its relay log, or time is up.
 *
 * @param new_master The new master
 * @param seconds_remaining How much time left
 * @param err_out Json error output
 * @return True if relay log was processed within time limit, or false if time ran out or an error occurred.
 */
bool MariaDBMonitor::failover_wait_relay_log(MXS_MONITORED_SERVER* new_master, int seconds_remaining,
                             json_t** err_out)
{
    MariaDBServer* master_info = get_server_info(new_master);
    time_t begin = time(NULL);
    bool query_ok = true;
    bool io_pos_stable = true;
    while (master_info->relay_log_events() > 0 &&
           query_ok &&
           io_pos_stable &&
           difftime(time(NULL), begin) < seconds_remaining)
    {
        MXS_INFO("Relay log of server '%s' not yet empty, waiting to clear %" PRId64 " events.",
                 new_master->server->unique_name, master_info->relay_log_events());
        thread_millisleep(1000); // Sleep for a while before querying server again.
        // Todo: check server version before entering failover.
        Gtid old_gtid_io_pos = master_info->slave_status.gtid_io_pos;
        // Update gtid:s first to make sure Gtid_IO_Pos is the more recent value.
        // It doesn't matter here, but is a general rule.
        query_ok = update_gtids(master_info) &&
                   do_show_slave_status(master_info, new_master);
        io_pos_stable = (old_gtid_io_pos == master_info->slave_status.gtid_io_pos);
    }

    bool rval = false;
    if (master_info->relay_log_events() == 0)
    {
        rval = true;
    }
    else
    {
        string reason = "Timeout";
        if (!query_ok)
        {
            reason = "Query error";
        }
        else if (!io_pos_stable)
        {
            reason = "Old master sent new event(s)";
        }
        else if (master_info->relay_log_events() < 0)
        {
            reason = "Invalid Gtid(s) (current_pos: " + master_info->gtid_current_pos.to_string() +
                     ", io_pos: " + master_info->slave_status.gtid_io_pos.to_string() + ")";
        }
        PRINT_MXS_JSON_ERROR(err_out, "Failover: %s while waiting for server '%s' to process relay log. "
                             "Cancelling failover.",
                             reason.c_str(), new_master->server->unique_name);
        rval = false;
    }
    return rval;
}

/**
 * Demotes the current master server, preparing it for replicating from another server. This step can take a
 * while if long writes are running on the server.
 *
 * @param current_master Server to demote
 * @param info Current master info. Will be written to. TODO: Remove need for this.
 * @param err_out json object for error printing. Can be NULL.
 * @return True if successful.
 */
bool MariaDBMonitor::switchover_demote_master(MXS_MONITORED_SERVER* current_master, MariaDBServer* info,
                                              json_t** err_out)
{
    MXS_NOTICE("Demoting server '%s'.", current_master->server->unique_name);
    bool success = false;
    bool query_error = false;
    MYSQL* conn = current_master->con;
    const char* query = ""; // The next query to execute. Used also for error printing.
    // The presence of an external master changes several things.
    const bool external_master = SERVER_IS_SLAVE_OF_EXTERNAL_MASTER(current_master->server);

    if (external_master)
    {
        // First need to stop slave. read_only is probably on already, although not certain.
        query = "STOP SLAVE;";
        query_error = (mxs_mysql_query(conn, query) != 0);
        if (!query_error)
        {
            query = "RESET SLAVE ALL;";
            query_error = (mxs_mysql_query(conn, query) != 0);
        }
    }

    bool error_fetched = false;
    string error_desc;
    if (!query_error)
    {
        query = "SET GLOBAL read_only=1;";
        query_error = (mxs_mysql_query(conn, query) != 0);
        if (!query_error)
        {
            // If have external master, no writes are allowed so skip this step. It's not essential, just
            // adds one to gtid.
            if (!external_master)
            {
                query = "FLUSH TABLES;";
                query_error = (mxs_mysql_query(conn, query) != 0);
            }

            if (!query_error)
            {
                query = "FLUSH LOGS;";
                query_error = (mxs_mysql_query(conn, query) != 0);
                if (!query_error)
                {
                    query = "";
                    if (update_gtids(info))
                    {
                        success = true;
                    }
                }
            }

            if (!success)
            {
                // Somehow, a step after "SET read_only" failed. Try to set read_only back to 0. It may not
                // work since the connection is likely broken.
                error_desc = mysql_error(conn); // Read connection error before next step.
                error_fetched = true;
                mxs_mysql_query(conn, "SET GLOBAL read_only=0;");
            }
        }
    }

    if (query_error && !error_fetched)
    {
        error_desc = mysql_error(conn);
    }

    if (!success)
    {
        if (query_error)
        {
            if (error_desc.empty())
            {
                const char UNKNOWN_ERROR[] = "Demotion failed due to an unknown error when executing "
                                             "a query. Query: '%s'.";
                PRINT_MXS_JSON_ERROR(err_out, UNKNOWN_ERROR, query);
            }
            else
            {
                const char KNOWN_ERROR[] = "Demotion failed due to a query error: '%s'. Query: '%s'.";
                PRINT_MXS_JSON_ERROR(err_out, KNOWN_ERROR, error_desc.c_str(), query);
            }
        }
        else
        {
            const char GTID_ERROR[] = "Demotion failed due to an error in updating gtid:s. "
                                      "Check log for more details.";
            PRINT_MXS_JSON_ERROR(err_out, GTID_ERROR);
        }
    }
    return success;
}

/**
 * Wait until slave replication catches up with the master gtid for all slaves in the vector.
 *
 * @param slave Slaves to wait on
 * @param gtid Which gtid must be reached
 * @param total_timeout Maximum wait time in seconds
 * @param read_timeout The value of read_timeout for the connection TODO: see if timeouts can be removed here
 * @param err_out json object for error printing. Can be NULL.
 * @return True, if target gtid was reached within allotted time for all servers
 */
bool MariaDBMonitor::switchover_wait_slaves_catchup(const ServerVector& slaves, const Gtid& gtid,
                                                    int total_timeout, int read_timeout, json_t** err_out)
{
    bool success = true;
    int seconds_remaining = total_timeout;

    for (ServerVector::const_iterator iter = slaves.begin();
         iter != slaves.end() && success;
         iter++)
    {
        if (seconds_remaining <= 0)
        {
            success = false;
        }
        else
        {
            time_t begin = time(NULL);
            MXS_MONITORED_SERVER* slave = *iter;
            if (switchover_wait_slave_catchup(slave, gtid, seconds_remaining, read_timeout, err_out))
            {
                seconds_remaining -= difftime(time(NULL), begin);
            }
            else
            {
                success = false;
            }
        }
    }
    return success;
}

/**
 * Wait until slave replication catches up with the master gtid
 *
 * @param slave Slave to wait on
 * @param gtid Which gtid must be reached
 * @param total_timeout Maximum wait time in seconds TODO: timeouts
 * @param read_timeout The value of read_timeout for the connection
 * @param err_out json object for error printing. Can be NULL.
 * @return True, if target gtid was reached within allotted time
 */
bool MariaDBMonitor::switchover_wait_slave_catchup(MXS_MONITORED_SERVER* slave, const Gtid& gtid,
                                          int total_timeout, int read_timeout,
                                          json_t** err_out)
{
    ss_dassert(read_timeout > 0);
    StringVector output;
    bool gtid_reached = false;
    bool error = false;
    double seconds_remaining = total_timeout;

    // Determine a reasonable timeout for the MASTER_GTID_WAIT-function depending on the
    // backend_read_timeout setting (should be >= 1) and time remaining.
    double loop_timeout = double(read_timeout) - 0.5;
    string cmd = gtid.generate_master_gtid_wait_cmd(loop_timeout);

    while (seconds_remaining > 0 && !gtid_reached && !error)
    {
        if (loop_timeout > seconds_remaining)
        {
            // For the last iteration, change the wait timeout.
            cmd = gtid.generate_master_gtid_wait_cmd(seconds_remaining);
        }
        seconds_remaining -= loop_timeout;

        if (query_one_row(slave, cmd.c_str(), 1, &output))
        {
            if (output[0] == "0")
            {
                gtid_reached = true;
            }
            output.clear();
        }
        else
        {
            error = true;
        }
    }

    if (error)
    {
        PRINT_MXS_JSON_ERROR(err_out, "MASTER_GTID_WAIT() query error on slave '%s'.",
                             slave->server->unique_name);
    }
    else if (!gtid_reached)
    {
        PRINT_MXS_JSON_ERROR(err_out, "MASTER_GTID_WAIT() timed out on slave '%s'.",
                             slave->server->unique_name);
    }
    return gtid_reached;
}

/**
 * Send an event to new master and wait for slaves to get the event.
 *
 * @param new_master Where to send the event
 * @param slaves Servers to monitor
 * @param seconds_remaining How long can we wait
 * @return True, if at least one slave got the new event within the time limit
 */
bool MariaDBMonitor::wait_cluster_stabilization(MXS_MONITORED_SERVER* new_master, const ServerVector& slaves,
                                                int seconds_remaining)
{
    ss_dassert(!slaves.empty());
    bool rval = false;
    time_t begin = time(NULL);
    MariaDBServer* new_master_info = get_server_info(new_master);

    if (mxs_mysql_query(new_master->con, "FLUSH TABLES;") == 0 &&
        update_gtids(new_master_info))
    {
        int query_fails = 0;
        int repl_fails = 0;
        int successes = 0;
        const Gtid target = new_master_info->gtid_current_pos;
        ServerVector wait_list = slaves; // Check all the servers in the list
        bool first_round = true;
        bool time_is_up = false;

        while (!wait_list.empty() && !time_is_up)
        {
            if (!first_round)
            {
                thread_millisleep(500);
            }

            // Erasing elements from an array, so iterate from last to first
            int i = wait_list.size() - 1;
            while (i >= 0)
            {
                MXS_MONITORED_SERVER* slave = wait_list[i];
                MariaDBServer* slave_info = get_server_info(slave);
                if (update_gtids(slave_info) && do_show_slave_status(slave_info, slave))
                {
                    if (!slave_info->slave_status.last_error.empty())
                    {
                        // IO or SQL error on slave, replication is a fail
                        MXS_WARNING("Slave '%s' cannot start replication: '%s'.",
                                    slave->server->unique_name,
                                    slave_info->slave_status.last_error.c_str());
                        wait_list.erase(wait_list.begin() + i);
                        repl_fails++;
                    }
                    else if (slave_info->gtid_current_pos.sequence >= target.sequence)
                    {
                        // This slave has reached the same gtid as master, remove from list
                        wait_list.erase(wait_list.begin() + i);
                        successes++;
                    }
                }
                else
                {
                    wait_list.erase(wait_list.begin() + i);
                    query_fails++;
                }
                i--;
            }

            first_round = false; // Sleep at start of next iteration
            if (difftime(time(NULL), begin) >= seconds_remaining)
            {
                time_is_up = true;
            }
        }

        ServerVector::size_type fails = repl_fails + query_fails + wait_list.size();
        if (fails > 0)
        {
            const char MSG[] = "Replication from the new master could not be confirmed for %lu slaves. "
                               "%d encountered an I/O or SQL error, %d failed to reply and %lu did not "
                               "advance in Gtid until time ran out.";
            MXS_WARNING(MSG, fails, repl_fails, query_fails, wait_list.size());
        }
        rval = (successes > 0);
    }
    else
    {
        MXS_ERROR("Could not confirm replication after switchover/failover because query to "
                  "the new master failed.");
    }
    return rval;
}

/**
 * Check that the given slave is a valid promotion candidate.
 *
 * @param preferred Preferred new master
 * @param err_out Json object for error printing. Can be NULL.
 * @return True, if given slave is a valid promotion candidate.
 */
bool MariaDBMonitor::switchover_check_preferred_master(MXS_MONITORED_SERVER* preferred, json_t** err_out)
{
    ss_dassert(preferred);
    bool rval = true;
    MariaDBServer* preferred_info = update_slave_info(preferred);
    if (preferred_info == NULL || !check_replication_settings(preferred, preferred_info))
    {
        PRINT_MXS_JSON_ERROR(err_out, "The requested server '%s' is not a valid promotion candidate.",
                             preferred->server->unique_name);
        rval = false;
    }
    return rval;
}

/**
 * Prepares a server for the replication master role.
 *
 * @param new_master The new master server
 * @param err_out json object for error printing. Can be NULL.
 * @return True if successful
 */
bool MariaDBMonitor::promote_new_master(MXS_MONITORED_SERVER* new_master, json_t** err_out)
{
    bool success = false;
    MXS_NOTICE("Promoting server '%s' to master.", new_master->server->unique_name);
    const char* query = "STOP SLAVE;";
    if (mxs_mysql_query(new_master->con, query) == 0)
    {
        query = "RESET SLAVE ALL;";
        if (mxs_mysql_query(new_master->con, query) == 0)
        {
            query = "SET GLOBAL read_only=0;";
            if (mxs_mysql_query(new_master->con, query) == 0)
            {
                success = true;
            }
        }
    }

    if (!success)
    {
        PRINT_MXS_JSON_ERROR(err_out, "Promotion failed: '%s'. Query: '%s'.",
                             mysql_error(new_master->con), query);
    }
    // If the previous master was a slave to an external master, start the equivalent slave connection on
    // the new master. Success of replication is not checked.
    else if (m_external_master_port != PORT_UNKNOWN && !start_external_replication(new_master, err_out))
    {
        success = false;
    }
    return success;
}

/**
 * Select a new master. Also add slaves which should be redirected to an array.
 *
 * @param out_slaves Vector for storing slave servers.
 * @param err_out json object for error printing. Can be NULL.
 * @return The found master, or NULL if not found
 */
MXS_MONITORED_SERVER* MariaDBMonitor::select_new_master(ServerVector* slaves_out, json_t** err_out)
{
    ss_dassert(slaves_out && slaves_out->size() == 0);
    /* Select a new master candidate. Selects the one with the latest event in relay log.
     * If multiple slaves have same number of events, select the one with most processed events. */
    MXS_MONITORED_SERVER* current_best = NULL;
    MariaDBServer* current_best_info = NULL;
     // Servers that cannot be selected because of exclusion, but seem otherwise ok.
    ServerVector valid_but_excluded;
    // Index of the current best candidate in slaves_out
    int master_vector_index = -1;

    for (MXS_MONITORED_SERVER *cand = m_monitor_base->monitored_servers; cand; cand = cand->next)
    {
        // If a server cannot be connected to, it won't be considered for promotion or redirected.
        // Do not worry about the exclusion list yet, querying the excluded servers is ok.
        MariaDBServer* cand_info = update_slave_info(cand);
        // If master is replicating from external master, it is updated but not added to array.
        if (cand_info && cand != m_master)
        {
            slaves_out->push_back(cand);
            // Check that server is not in the exclusion list while still being a valid choice.
            if (server_is_excluded(cand) && check_replication_settings(cand, cand_info, WARNINGS_OFF))
            {
                valid_but_excluded.push_back(cand);
                const char CANNOT_SELECT[] = "Promotion candidate '%s' is excluded from new "
                "master selection.";
                MXS_INFO(CANNOT_SELECT, cand->server->unique_name);
            }
            else if (check_replication_settings(cand, cand_info))
            {
                // If no new master yet, accept any valid candidate. Otherwise check.
                if (current_best == NULL || is_candidate_better(current_best_info, cand_info))
                {
                    // The server has been selected for promotion, for now.
                    current_best = cand;
                    current_best_info = cand_info;
                    master_vector_index = slaves_out->size() - 1;
                }
            }
        }
    }

    if (current_best)
    {
        // Remove the selected master from the vector.
        ServerVector::iterator remove_this = slaves_out->begin();
        remove_this += master_vector_index;
        slaves_out->erase(remove_this);
    }

    // Check if any of the excluded servers would be better than the best candidate.
    for (ServerVector::const_iterator iter = valid_but_excluded.begin();
         iter != valid_but_excluded.end();
         iter++)
    {
        MariaDBServer* excluded_info = get_server_info(*iter);
        const char* excluded_name = (*iter)->server->unique_name;
        if (current_best == NULL)
        {
            const char EXCLUDED_ONLY_CAND[] = "Server '%s' is a viable choice for new master, "
            "but cannot be selected as it's excluded.";
            MXS_WARNING(EXCLUDED_ONLY_CAND, excluded_name);
            break;
        }
        else if (is_candidate_better(current_best_info, excluded_info))
        {
            // Print a warning if this server is actually a better candidate than the previous
            // best.
            const char EXCLUDED_CAND[] = "Server '%s' is superior to current "
            "best candidate '%s', but cannot be selected as it's excluded. This may lead to "
            "loss of data if '%s' is ahead of other servers.";
            MXS_WARNING(EXCLUDED_CAND, excluded_name, current_best->server->unique_name, excluded_name);
            break;
        }
    }

    if (current_best == NULL)
    {
        PRINT_MXS_JSON_ERROR(err_out, "No suitable promotion candidate found.");
    }
    return current_best;
}

/**
 * Is the server in the excluded list
 *
 * @param handle Cluster monitor
 * @param server Server to test
 * @return True if server is in the excluded-list of the monitor.
 */
bool MariaDBMonitor::server_is_excluded(const MXS_MONITORED_SERVER* server)
{
    size_t n_excluded = m_excluded_servers.size();
    for (size_t i = 0; i < n_excluded; i++)
    {
        if (m_excluded_servers[i] == server)
        {
            return true;
        }
    }
    return false;
}

/**
 * Is the candidate a better choice for master than the previous best?
 *
 * @param current_best_info Server info of current best choice
 * @param candidate_info Server info of new candidate
 * @return True if candidate is better
 */
bool MariaDBMonitor::is_candidate_better(const MariaDBServer* current_best_info,
                                         const MariaDBServer* candidate_info)
{
    uint64_t cand_io = candidate_info->slave_status.gtid_io_pos.sequence;
    uint64_t cand_processed = candidate_info->gtid_current_pos.sequence;
    uint64_t curr_io = current_best_info->slave_status.gtid_io_pos.sequence;
    uint64_t curr_processed = current_best_info->gtid_current_pos.sequence;
    bool cand_updates = candidate_info->rpl_settings.log_slave_updates;
    bool curr_updates = current_best_info->rpl_settings.log_slave_updates;
    bool is_better = false;
    // Accept a slave with a later event in relay log.
    if (cand_io > curr_io)
    {
        is_better = true;
    }
    // If io sequences are identical, the slave with more events processed wins.
    else if (cand_io == curr_io)
    {
        if (cand_processed > curr_processed)
        {
            is_better = true;
        }
        // Finally, if binlog positions are identical, prefer a slave with log_slave_updates.
        else if (cand_processed == curr_processed && cand_updates && !curr_updates)
        {
            is_better = true;
        }
    }
    return is_better;
}

/**
 * Check that the given server is a master and it's the only master.
 *
 * @param suggested_curr_master     The server to check, given by user.
 * @param error_out                 On output, error object if function failed.
 * @return True if current master seems ok. False, if there is some error with the
 * specified current master.
 */
bool MariaDBMonitor::switchover_check_current(const MXS_MONITORED_SERVER* suggested_curr_master,
                                              json_t** error_out) const
{
    ss_dassert(suggested_curr_master);
    bool server_is_master = false;
    MXS_MONITORED_SERVER* extra_master = NULL; // A master server which is not the suggested one
    for (MXS_MONITORED_SERVER* mon_serv = m_monitor_base->monitored_servers;
         mon_serv != NULL && extra_master == NULL;
         mon_serv = mon_serv->next)
    {
        if (SERVER_IS_MASTER(mon_serv->server))
        {
            if (mon_serv == suggested_curr_master)
            {
                server_is_master = true;
            }
            else
            {
                extra_master = mon_serv;
            }
        }
    }

    if (!server_is_master)
    {
        PRINT_MXS_JSON_ERROR(error_out, "Server '%s' is not the current master or it's in maintenance.",
                             suggested_curr_master->server->unique_name);
    }
    else if (extra_master)
    {
        PRINT_MXS_JSON_ERROR(error_out, "Cluster has an additional master server '%s'.",
                             extra_master->server->unique_name);
    }
    return server_is_master && !extra_master;
}

/**
 * Check whether specified new master is acceptable.
 *
 * @param monitored_server      The server to check against.
 * @param error                 On output, error object if function failed.
 *
 * @return True, if suggested new master is a viable promotion candidate.
 */
bool MariaDBMonitor::switchover_check_new(const MXS_MONITORED_SERVER* monitored_server, json_t** error)
{
    SERVER* server = monitored_server->server;
    const char* name = server->unique_name;
    bool is_master = SERVER_IS_MASTER(server);
    bool is_slave = SERVER_IS_SLAVE(server);

    if (is_master)
    {
        const char IS_MASTER[] = "Specified new master '%s' is already the current master.";
        PRINT_MXS_JSON_ERROR(error, IS_MASTER, name);
    }
    else if (!is_slave)
    {
        const char NOT_SLAVE[] = "Specified new master '%s' is not a slave.";
        PRINT_MXS_JSON_ERROR(error, NOT_SLAVE, name);
    }

    return !is_master && is_slave;
}

/**
 * Check that preconditions for a failover are met.
 *
 * @param error_out JSON error out
 * @return True if failover may proceed
 */
bool MariaDBMonitor::failover_check(json_t** error_out)
{
    // Check that there is no running master and that there is at least one running server in the cluster.
    // Also, all slaves must be using gtid-replication.
    int slaves = 0;
    bool error = false;

    for (MXS_MONITORED_SERVER* mon_server = m_monitor_base->monitored_servers;
         mon_server != NULL;
         mon_server = mon_server->next)
    {
        uint64_t status_bits = mon_server->server->status;
        uint64_t master_up = (SERVER_MASTER | SERVER_RUNNING);
        if ((status_bits & master_up) == master_up)
        {
            string master_up_msg = string("Master server '") + mon_server->server->unique_name +
                                   "' is running";
            if (status_bits & SERVER_MAINT)
            {
                master_up_msg += ", although in maintenance mode";
            }
            master_up_msg += ".";
            PRINT_MXS_JSON_ERROR(error_out, "%s", master_up_msg.c_str());
            error = true;
        }
        else if (SERVER_IS_SLAVE(mon_server->server))
        {
            if (uses_gtid(mon_server, error_out))
            {
                 slaves++;
            }
            else
            {
                 error = true;
            }
        }
    }

    if (error)
    {
        PRINT_MXS_JSON_ERROR(error_out, "Failover not allowed due to errors.");
    }
    else if (slaves == 0)
    {
        PRINT_MXS_JSON_ERROR(error_out, "No running slaves, cannot failover.");
    }
    return !error && slaves > 0;
}

/**
 * Check if server has binary log enabled. Print warnings if gtid_strict_mode or log_slave_updates is off.
 *
 * @param server Server to check
 * @param server_info Server info
 * @param print_on Print warnings or not
 * @return True if log_bin is on
 */
bool check_replication_settings(const MXS_MONITORED_SERVER* server, MariaDBServer* server_info,
                                print_repl_warnings_t print_warnings)
{
    bool rval = true;
    const char* servername = server->server->unique_name;
    if (server_info->rpl_settings.log_bin == false)
    {
        if (print_warnings == WARNINGS_ON)
        {
            const char NO_BINLOG[] =
                "Slave '%s' has binary log disabled and is not a valid promotion candidate.";
            MXS_WARNING(NO_BINLOG, servername);
        }
        rval = false;
    }
    else if (print_warnings == WARNINGS_ON)
    {
        if (server_info->rpl_settings.gtid_strict_mode == false)
        {
            const char NO_STRICT[] =
                "Slave '%s' has gtid_strict_mode disabled. Enabling this setting is recommended. "
                "For more information, see https://mariadb.com/kb/en/library/gtid/#gtid_strict_mode";
            MXS_WARNING(NO_STRICT, servername);
        }
        if (server_info->rpl_settings.log_slave_updates == false)
        {
            const char NO_SLAVE_UPDATES[] =
                "Slave '%s' has log_slave_updates disabled. It is a valid candidate but replication "
                "will break for lagging slaves if '%s' is promoted.";
            MXS_WARNING(NO_SLAVE_UPDATES, servername, servername);
        }
    }
    return rval;
}

/**
 * Checks if slave can replicate from master. Only considers gtid:s and only detects obvious errors. The
 * non-detected errors will mostly be detected once the slave tries to start replicating.
 *
 * @param slave Slave server candidate
 * @param slave_info Slave info
 * @param master_info Master info
 * @return True if slave can replicate from master
 */
bool MariaDBMonitor::can_replicate_from(MXS_MONITORED_SERVER* slave,
                                        MariaDBServer* slave_info, MariaDBServer* master_info)
{
    bool rval = false;
    if (update_gtids(slave_info))
    {
        Gtid slave_gtid = slave_info->gtid_current_pos;
        Gtid master_gtid = master_info->gtid_binlog_pos;
        // The following are not sufficient requirements for replication to work, they only cover the basics.
        // If the servers have diverging histories, the redirection will seem to succeed but the slave IO
        // thread will stop in error.
        if (slave_gtid.server_id != SERVER_ID_UNKNOWN && master_gtid.server_id != SERVER_ID_UNKNOWN &&
            slave_gtid.domain == master_gtid.domain &&
            slave_gtid.sequence <= master_info->gtid_current_pos.sequence)
        {
            rval = true;
        }
    }
    return rval;
}

/**
 * @brief Process possible failover event
 *
 * If a master failure has occurred and MaxScale is configured with failover functionality, this fuction
 * executes failover to select and promote a new master server. This function should be called immediately
 * after @c mon_process_state_changes.
 *
 * @param cluster_modified_out Set to true if modifying cluster
 * @return True on success, false on error
*/
bool MariaDBMonitor::mon_process_failover(bool* cluster_modified_out)
{
    ss_dassert(*cluster_modified_out == false);
    if (config_get_global_options()->passive ||
        (m_master && SERVER_IS_MASTER(m_master->server)))
    {
        return true;
    }
    bool rval = true;
    MXS_MONITORED_SERVER* failed_master = NULL;

    for (MXS_MONITORED_SERVER *ptr = m_monitor_base->monitored_servers; ptr; ptr = ptr->next)
    {
        if (ptr->new_event && ptr->server->last_event == MASTER_DOWN_EVENT)
        {
            if (failed_master)
            {
                MXS_ALERT("Multiple failed master servers detected: "
                          "'%s' is the first master to fail but server "
                          "'%s' has also triggered a master_down event.",
                          failed_master->server->unique_name,
                          ptr->server->unique_name);
                return false;
            }

            if (ptr->server->active_event)
            {
                // MaxScale was active when the event took place
                failed_master = ptr;
            }
            else if (m_monitor_base->master_has_failed)
            {
                /**
                 * If a master_down event was triggered when this MaxScale was
                 * passive, we need to execute the failover script again if no new
                 * masters have appeared.
                 */
                int64_t timeout = SEC_TO_HB(m_failover_timeout);
                int64_t t = hkheartbeat - ptr->server->triggered_at;

                if (t > timeout)
                {
                    MXS_WARNING("Failover of server '%s' did not take place within "
                                "%u seconds, failover needs to be re-triggered",
                                ptr->server->unique_name, m_failover_timeout);
                    failed_master = ptr;
                }
            }
        }
    }

    if (failed_master)
    {
        if (m_failcount > 1 && failed_master->mon_err_count == 1)
        {
            MXS_WARNING("Master has failed. If master status does not change in %d monitor passes, failover "
                        "begins.", m_failcount - 1);
        }
        else if (failed_master->mon_err_count >= m_failcount)
        {
            MXS_NOTICE("Performing automatic failover to replace failed master '%s'.",
                       failed_master->server->unique_name);
            failed_master->new_event = false;
            rval = failover_check(NULL) && do_failover(NULL);
            if (rval)
            {
                *cluster_modified_out = true;
            }
        }
    }
    return rval;
}

/**
 * Print a redirect error to logs. If err_out exists, generate a combined error message by querying all
 * the server parameters for connection errors and append these errors to err_out.
 *
 * @param demotion_target If not NULL, this is the first server to query.
 * @param redirectable_slaves Other servers to query for errors.
 * @param err_out If not null, the error output object.
 */
void print_redirect_errors(MXS_MONITORED_SERVER* first_server, const ServerVector& servers,
                           json_t** err_out)
{
    // Individual server errors have already been printed to the log.
    // For JSON, gather the errors again.
    const char MSG[] = "Could not redirect any slaves to the new master.";
    MXS_ERROR(MSG);
    if (err_out)
    {
        ServerVector failed_slaves;
        if (first_server)
        {
            failed_slaves.push_back(first_server);
        }
        failed_slaves.insert(failed_slaves.end(),
                             servers.begin(), servers.end());
        string combined_error = get_connection_errors(failed_slaves);
        *err_out = mxs_json_error_append(*err_out,
                                         "%s Errors: %s.", MSG, combined_error.c_str());
    }
}

bool MariaDBMonitor::uses_gtid(MXS_MONITORED_SERVER* mon_server, json_t** error_out)
{
    bool rval = false;
    const MariaDBServer* info = get_server_info(mon_server);
    if (info->slave_status.gtid_io_pos.server_id == SERVER_ID_UNKNOWN)
    {
        string slave_not_gtid_msg = string("Slave server ") + mon_server->server->unique_name +
                                    " is not using gtid replication.";
        PRINT_MXS_JSON_ERROR(error_out, "%s", slave_not_gtid_msg.c_str());
    }
    else
    {
        rval = true;
    }
    return rval;
}

bool MariaDBMonitor::failover_not_possible()
{
    bool rval = false;

    for (MXS_MONITORED_SERVER* s = m_monitor_base->monitored_servers; s; s = s->next)
    {
        MariaDBServer* info = get_server_info(s);

        if (info->n_slaves_configured > 1)
        {
            MXS_ERROR("Server '%s' is configured to replicate from multiple "
                      "masters, failover is not possible.", s->server->unique_name);
            rval = true;
        }
    }

    return rval;
}

/**
 * Check if a slave is receiving events from master.
 *
 * @return True, if a slave has an event more recent than master_failure_timeout.
 */
bool MariaDBMonitor::slave_receiving_events()
{
    ss_dassert(m_master);
    bool received_event = false;
    int64_t master_id = m_master->server->node_id;
    for (MXS_MONITORED_SERVER* server = m_monitor_base->monitored_servers; server; server = server->next)
    {
        MariaDBServer* info = get_server_info(server);

        if (info->slave_configured &&
            info->slave_status.slave_io_running &&
            info->slave_status.master_server_id == master_id &&
            difftime(time(NULL), info->latest_event) < m_master_failure_timeout)
        {
            /**
             * The slave is still connected to the correct master and has received events. This means that
             * while MaxScale can't connect to the master, it's probably still alive.
             */
            received_event = true;
            break;
        }
    }
    return received_event;
}
