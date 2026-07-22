#ifndef QUERY_PARSER_H
#define QUERY_PARSER_H

#include <string>
#include <vector>
#include <sstream>

class QueryParser {
public:
    std::string query_table;
    std::vector<std::string> query_columns;
    std::string condition_column;
    std::string condition_value;
    bool is_count_query;

    QueryParser(std::string command) {
        is_count_query = false;
        parse(command);
    }

private:
    void parse(std::string command) {
        if (command.find("count(*)") != std::string::npos) {
            is_count_query = true;
            size_t from_pos = command.find("FROM ");
            if(from_pos != std::string::npos) {
                query_table = command.substr(from_pos + 5);
            }
            return;
        }

        size_t select_pos = command.find("SELECT ");
        size_t from_pos = command.find(" FROM ");
        size_t where_pos = command.find(" WHERE ");

        if (select_pos != std::string::npos && from_pos != std::string::npos) {
            std::string col_part = command.substr(select_pos + 7, from_pos - (select_pos + 7));
            query_table = command.substr(from_pos + 6, (where_pos != std::string::npos ? where_pos - (from_pos + 6) : std::string::npos));

            std::stringstream ss(col_part);
            std::string col;
            while (std::getline(ss, col, ',')) {
                col.erase(0, col.find_first_not_of(" "));
                col.erase(col.find_last_not_of(" ") + 1);
                query_columns.push_back(col);
            }
        }

        if (where_pos != std::string::npos) {
            std::string cond_part = command.substr(where_pos + 7);
            size_t eq_pos = cond_part.find(" = ");
            if (eq_pos != std::string::npos) {
                condition_column = cond_part.substr(0, eq_pos);
                condition_value = cond_part.substr(eq_pos + 4);
                if (!condition_value.empty() && condition_value.back() == '\'') condition_value.pop_back();
                if (!condition_value.empty() && condition_value.front() == '\'') condition_value.erase(0, 1);
            }
        }
    }
};

#endif