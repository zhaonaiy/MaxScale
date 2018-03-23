#pragma once

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

#include <maxscale/cppdefs.hh>
#include <string>
#include <vector>

#include <maxscale/monitor.h>

/** Utility macro for printing both MXS_ERROR and json error */
#define PRINT_MXS_JSON_ERROR(err_out, format, ...)\
    do {\
       MXS_ERROR(format, ##__VA_ARGS__);\
       if (err_out)\
       {\
            *err_out = mxs_json_error_append(*err_out, format, ##__VA_ARGS__);\
       }\
    } while (false)

using std::string;

typedef std::vector<string> StringVector;
typedef std::vector<MXS_MONITORED_SERVER*> ServerVector;

extern const int64_t SERVER_ID_UNKNOWN;

/**
 * Scan a server id from a string.
 *
 * @param id_string
 * @return Server id, or -1 if scanning fails
 */
int64_t scan_server_id(const char* id_string);

/**
 * Query one row of results, save strings to array. Any additional rows are ignored.
 *
 * @param database The database to query.
 * @param query The query to execute.
 * @param expected_cols How many columns the result should have.
 * @param output The output array to populate.
 * @return True on success.
 */
bool query_one_row(MXS_MONITORED_SERVER *database, const char* query, unsigned int expected_cols,
                   StringVector* output);

/**
 * Get MariaDB connection error strings from all the given servers, form one string.
 *
 * @param slaves Servers with errors
 * @return Concatenated string.
 */
string get_connection_errors(const ServerVector& servers);

/**
 * Generates a list of server names separated by ', '
 *
 * @param array The servers
 * @return Server names
 */
string monitored_servers_to_string(const ServerVector& array);

/**
 * Helper class for simplifying working with resultsets. Used in MariaDBServer.
 */
class QueryResult
{
private:
    QueryResult(const QueryResult& source) = delete;
    QueryResult& operator = (const QueryResult& source) = delete;
private:
    std::tr1::unordered_map<string, int64_t> m_col_indexes; // Map of column name -> index
    std::vector<string> m_data; // Data array
    int64_t m_current_row; // From which row are results currently returned
    int64_t m_columns; // How many columns does the data have. Usually equal to column index map size.
    
public:
    int tarkistus;
    QueryResult();
    ~QueryResult();
    QueryResult(QueryResult&& source) noexcept;
    /**
     * Read a result set into the object. Discards any old data.
     *
     * @param results Resultset obtained by @c mysql_store_result() i.e. buffered.
     * Do not try with @c mysql_use_result().
     * @param columns Number of columns, obtained from the connection with @c mysql_field_count().
     * The column names should be unique, otherwise @c get_col_index() will give wrong results.
     * @return True if successful and column names were unique. Even if false, the set may contain data.
     */
    bool insert_data(MYSQL_RES* resultset);

    bool has_data();
    /**
     * Advance to next row. Affects all result returning functions.
     *
     * @return The index of the next row. If no next row, returns -1.
     */
    int64_t next_row();

    /**
     * Checks if there are more rows.
     *
     * @return True if more rows.
     */
    bool has_next_row();

    /**
     * Get a numeric index for a column name.
     *
     * @param col_name Column name
     * @return Index or -1 if not found.
     */
    int64_t get_col_index(const string& col_name);

    /**
     * Read a string value from the current row and given column.
     *
     * @param column_ind Column index
     * @return Value as string
     */
    string get_string(int64_t column_ind);

    /**
     * Read an integer value from the current row and given column. No error checking is done on the parsing.
     * The parsing is performed by @c strtoll(), so the caller may check errno for errors.
     *
     * @param column_ind Column index
     * @return Value as integer
     */
    int64_t get_int(int64_t column_ind);

    /**
     * Read a boolean value from the current row and given column.
     *
     * @param column_ind Column index
     * @return Value as boolean. Returns true if the text is either 'Y' or '1'.
     */
    bool get_bool(int64_t column_ind);
};
