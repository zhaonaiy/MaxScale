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

#include "utilities.hh"

#include <inttypes.h>
#include <limits>
#include <stdio.h>
#include <string>
#include <sstream>
#include <maxscale/dcb.h>
#include <maxscale/debug.h>
#include <maxscale/mysql_utils.h>

/** Server id default value */
const int64_t SERVER_ID_UNKNOWN = -1;

int64_t scan_server_id(const char* id_string)
{
    int64_t server_id = SERVER_ID_UNKNOWN;
    ss_debug(int rv = ) sscanf(id_string, "%" PRId64, &server_id);
    ss_dassert(rv == 1);
    // Server id can be 0, which was even the default value until 10.2.1.
    // KB is a bit hazy on this, but apparently when replicating, the server id should not be 0. Not sure,
    // so MaxScale allows this.
#if defined(SS_DEBUG)
    const int64_t SERVER_ID_MIN = std::numeric_limits<uint32_t>::min();
    const int64_t SERVER_ID_MAX = std::numeric_limits<uint32_t>::max();
#endif
    ss_dassert(server_id >= SERVER_ID_MIN && server_id <= SERVER_ID_MAX);
    return server_id;
}

bool query_one_row(MXS_MONITORED_SERVER *database, const char* query, unsigned int expected_cols,
                   StringVector* output)
{
    bool rval = false;
    MYSQL_RES *result;
    if (mxs_mysql_query(database->con, query) == 0 && (result = mysql_store_result(database->con)) != NULL)
    {
        unsigned int columns = mysql_field_count(database->con);
        if (columns != expected_cols)
        {
            mysql_free_result(result);
            MXS_ERROR("Unexpected result for '%s'. Expected %d columns, got %d. Server version: %s",
                      query, expected_cols, columns, database->server->version_string);
        }
        else
        {
            MYSQL_ROW row = mysql_fetch_row(result);
            if (row)
            {
                for (unsigned int i = 0; i < columns; i++)
                {
                    output->push_back((row[i] != NULL) ? row[i] : "");
                }
                rval = true;
            }
            else
            {
                MXS_ERROR("Query '%s' returned no rows.", query);
            }
            mysql_free_result(result);
        }
    }
    else
    {
        mon_report_query_error(database);
    }
    return rval;
}

string get_connection_errors(const ServerVector& servers)
{
    // Get errors from all connections, form a string.
    std::stringstream ss;
    for (ServerVector::const_iterator iter = servers.begin(); iter != servers.end(); iter++)
    {
        const char* error = mysql_error((*iter)->con);
        ss_dassert(*error); // Every connection should have an error.
        ss << (*iter)->server->unique_name << ": '" << error << "'";
        if (iter + 1 != servers.end())
        {
            ss << ", ";
        }
    }
    return ss.str();
}

string monitored_servers_to_string(const ServerVector& array)
{
    string rval;
    size_t array_size = array.size();
    if (array_size > 0)
    {
        const char* separator = "";
        for (size_t i = 0; i < array_size; i++)
        {
            rval += separator;
            rval += array[i]->server->unique_name;
            separator = ",";
        }
    }
    return rval;
}

bool QueryResult::insert_data(MYSQL_RES* resultset)
{
    m_current_row = 0;
    m_col_indexes.clear();
    m_data.clear();
    m_columns = mysql_num_fields(resultset);

    MYSQL_FIELD* field_info = mysql_fetch_fields(resultset);
    bool error = false; // Even if an error occurs, keep reading as much as possible.

    for (int64_t column_index = 0; column_index < m_columns; column_index++)
    {
        string key(field_info[column_index].name);
        if (m_col_indexes.count(key) == 0)
        {
            m_col_indexes[key] = column_index;
        }
        else
        {
            error = true;
            MXS_ERROR("Duplicate column name in result set detected: '%s'.", key.c_str());
        }
    }

    // Fill in data to array. Use the vector as a 2D array even though it's just a 1-D vector.
    const my_ulonglong rows = mysql_num_rows(resultset);
    m_data.resize(m_columns * rows); // Initializes all elements to empty strings.

    my_ulonglong row_index = 0;
    while (row_index < rows)
    {
        MYSQL_ROW row = mysql_fetch_row(resultset);
        if (row)
        {
            for (int64_t column_index = 0; column_index < m_columns; column_index++)
            {
                // Empty strings and NULL values are identical.
                const char* element = row[column_index];
                if (element)
                {
                    m_data[(row_index * m_columns) + column_index] = element;
                }
            }
        }
        else
        {
            MXS_ERROR("Not enough rows: expected %llu but only got %llu.", rows, row_index);
            m_data.resize(m_columns * row_index);
            error = true;
            break;
        }
        row_index++;
    }
    return !error;
}

int64_t QueryResult::next_row()
{
    return (has_next_row()) ? (++m_current_row) : -1;
}

bool QueryResult::has_next_row()
{
    int64_t rows = m_data.size() / m_columns;
    return (m_current_row + 1 < rows); // current_row is an index, rows is a count.
}

int64_t QueryResult::get_col_index(const string& col_name)
{
    auto iter = m_col_indexes.find(col_name);
    return (iter != m_col_indexes.end()) ? iter->second : -1;
}

string QueryResult::get_string(int64_t column_ind)
{
    ss_dassert(column_ind < m_columns);
    return m_data[m_current_row * m_columns + column_ind];
}

int64_t QueryResult::get_int(int64_t column_ind)
{
    ss_dassert(column_ind < m_columns);
    string& data = m_data[m_current_row * m_columns + column_ind];
    errno = 0; // strtoll sets this
    return strtoll(data.c_str(), NULL, 10);
}

bool QueryResult::get_bool(int64_t column_ind)
{
    ss_dassert(column_ind < m_columns);
    string& data = m_data[m_current_row * m_columns + column_ind];
    return (data == "Y" || data == "1");
}
