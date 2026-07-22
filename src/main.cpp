#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cassert>
#include "../utils.h"
#include "../query_parser.h"
#include <unordered_map>

using namespace std;

const int DB_HEADER_SIZE = 100;

unordered_map<string, int> parse_schema_to_map(const string& create_table_sql) {
    unordered_map<string, int> schema;
    size_t start = create_table_sql.find('(');
    if (start == string::npos) return schema;

    string cols_str = create_table_sql.substr(start + 1);
    size_t end = cols_str.rfind(')');
    if (end != string::npos) cols_str = cols_str.substr(0, end);

    stringstream ss(cols_str);
    string part;
    int idx = 0;

    while (getline(ss, part, ',')) {
        // Trim leading whitespace
        size_t first = part.find_first_not_of(" \t\r\n");
        if (first == string::npos) continue;
        string trimmed = part.substr(first);

        // Grab the column name (the first word)
        size_t space = trimmed.find_first_of(" \t\r\n");
        string col_name = (space == string::npos) ? trimmed : trimmed.substr(0, space);

        // Strip any SQL schema wrappers (e.g. "name" or [name])
        if (!col_name.empty() && (col_name.front() == '"' || col_name.front() == '\'' || col_name.front() == '`' || col_name.front() == '[')) col_name.erase(0, 1);
        if (!col_name.empty() && (col_name.back() == '"' || col_name.back() == '\'' || col_name.back() == '`' || col_name.back() == ']')) col_name.pop_back();

        schema[col_name] = idx++;
    }
    return schema;
}

class DB {
public:
    ifstream stream;
    unsigned short page_size,cell_count;

    DB(string db_file_path) {
        this->stream = ifstream(db_file_path, ios::binary);
        if (!this->stream.is_open()) {
            cerr << "Failed to open database file." << endl;
            exit(1);
        }
        
        // Extract Page Size
        this->stream.seekg(16);
        this->page_size = big_endian(this->stream, 2);

        this->stream.seekg(103);
        this->cell_count = big_endian(stream, 2);
    }

    void print_table_names() {
        // Page 1 contains the sqlite_schema.
        // The cell count is located at byte offset 3 of the B-Tree page header.
        // Since Page 1 has a 100-byte DB header, the cell count is at 100 + 3 = 103.

        vector<string> table_names;

        // The cell pointer array starts immediately after the 8-byte B-Tree header.
        // For Page 1, this is 100 (DB Header) + 8 (B-Tree Header) = 108.
        for (int i = 0; i < cell_count; i++) {
            stream.seekg(108 + (i * 2));
            unsigned short cell_offset = big_endian(stream, 2);

            // On Page 1, the page offset is 0, so the absolute offset is exactly the cell_offset.
            stream.seekg(cell_offset);

            // 1. Skip Payload Size (Varint)
            parse_varint(stream);
            
            // 2. Skip RowID (Varint)
            parse_varint(stream);

            // 3. Parse the Record
            vector<string> row = parse_record(stream);

            // The sqlite_schema columns are: 
            // 0: type, 1: name, 2: tbl_name, 3: rootpage, 4: sql
            if (row.size() >= 3 && row[0] == "table" && row[1] != "sqlite_sequence") {
                table_names.push_back(row[2]);
            }
        }

        // Print tables separated by spaces
        for (size_t i = 0; i < table_names.size(); i++) {
            cout << table_names[i] << (i == table_names.size() - 1 ? "" : " ");
        }
        cout << endl;
    }
   // 1. Search the schema table for a specific table name and return its root page
    int get_table_root_page(string target_table_name) {
        for (int i = 0; i < cell_count; i++) {
            stream.seekg(108 + (i * 2));
            unsigned short cell_offset = big_endian(stream, 2);
            stream.seekg(cell_offset);

            parse_varint(stream); // skip payload size
            parse_varint(stream); // skip rowid

            vector<string> row = parse_record(stream);

            // columns: 0:type, 1:name, 2:tbl_name, 3:rootpage, 4:sql
            if (row.size() >= 4 && row[0] == "table" && row[2] == target_table_name) {
                return stoi(row[3]); // Convert the rootpage string to an integer
            }
        }
        cerr << "Table not found in schema: " << target_table_name << endl;
        exit(1);
    }

    // 2. Jump to any page and read the 2-byte cell count sitting at offset +3
    unsigned short get_page_cell_count(int page_number) {
        // Apply the Golden Rules: Page 1 pays 100 tax, Page N pays (N-1) * page_size
        size_t page_offset = (page_number == 1) ? 100 : ((page_number - 1) * this->page_size);
        
        stream.seekg(page_offset + 3);
        return big_endian(stream, 2);
    }

    // 1. Upgraded metadata fetcher: returns { root_page_as_int, create_table_sql_string }
    pair<int, string> get_table_metadata(string target_table_name) {
        for (int i = 0; i < cell_count; i++) {
            stream.seekg(108 + (i * 2));
            unsigned short cell_offset = big_endian(stream, 2);
            stream.seekg(cell_offset);

            parse_varint(stream); // skip payload size
            parse_varint(stream); // skip rowid

            vector<string> row = parse_record(stream);

            // Columns: 0:type, 1:name, 2:tbl_name, 3:rootpage, 4:sql
            if (row.size() >= 5 && row[0] == "table" && row[2] == target_table_name) {
                return { stoi(row[3]), row[4] };
            }
        }
        cerr << "Table not found: " << target_table_name << endl;
        exit(1);
    }

    // 2. Scan a target page, parse every row, and print the requested column index
    // Added default parameters: filter_col_idx = -1 (meaning "no filter")
    // Replaced print_projected_rows with a universal recursive B-Tree walker
    void print_projected_rows(int page_number, const vector<int>& col_indices, int filter_col_idx = -1, string filter_val = "") {
        size_t page_start = (page_number - 1) * this->page_size;
        size_t header_tax = (page_number == 1) ? 100 : 0;
        size_t btree_header_start = page_start + header_tax;

        stream.seekg(btree_header_start);
        unsigned char page_type;
        stream.read((char*)&page_type, 1);

        if (page_type == 0x0D) {
            // ==========================================================
            //              CASE 1: LEAF PAGE (Payload Data)
            // ==========================================================
            stream.seekg(btree_header_start + 3);
            unsigned short page_cells = big_endian(stream, 2);
            size_t cell_ptr_array_start = btree_header_start + 8;

            for (int i = 0; i < page_cells; i++) {
                stream.seekg(cell_ptr_array_start + (i * 2));
                unsigned short cell_offset = big_endian(stream, 2);
                
                stream.seekg(page_start + cell_offset);

                parse_varint(stream); // skip payload size

                // 1. STOP SKIPPING THIS! Capture the B-Tree RowID into a variable
                int64_t row_id = parse_varint(stream).first; 

                vector<string> row = parse_record(stream);

                // 2. The SQLite RowID Alias Patch:
                // If column 0 (id) came back as an optimized NULL, overwrite it with our captured RowID
                if (!row.empty()) {
                    string& col_zero = row[0];
                    if (col_zero.empty() || col_zero == "NULL" || col_zero == "null" || col_zero == "None") {
                        col_zero = to_string(row_id);
                    }
                }

                // WHERE clause gatekeeper... (leave the rest of your loop untouched)

                // WHERE clause gatekeeper
                if (filter_col_idx != -1) {
                    if (filter_col_idx >= row.size() || row[filter_col_idx] != filter_val) continue;
                }

                for (size_t k = 0; k < col_indices.size(); k++) {
                    int c_idx = col_indices[k];
                    cout << (c_idx < row.size() ? row[c_idx] : ""); 
                    if (k < col_indices.size() - 1) cout << "|";
                }
                cout << endl;
            }
        }
        else if (page_type == 0x05) {
            // ==========================================================
            //            CASE 2: INTERIOR PAGE (Navigation Signposts)
            // ==========================================================
            stream.seekg(btree_header_start + 3);
            unsigned short page_cells = big_endian(stream, 2);

            // An interior page header is 12 bytes long.
            // Bytes 8, 9, 10, 11 hold the 4-byte Rightmost Child Page Pointer!
            stream.seekg(btree_header_start + 8);
            unsigned int rightmost_child_page = big_endian(stream, 4);

            size_t cell_ptr_array_start = btree_header_start + 12;

            for (int i = 0; i < page_cells; i++) {
                stream.seekg(cell_ptr_array_start + (i * 2));
                unsigned short cell_offset = big_endian(stream, 2);

                // Seek to the cell. In an interior table B-tree, the very first 
                // 4 bytes of the cell are the Left Child Page Pointer.
                stream.seekg(page_start + cell_offset);
                unsigned int left_child_page = big_endian(stream, 4);

                // RECURSION: Dive down into the left child
                print_projected_rows(left_child_page, col_indices, filter_col_idx, filter_val);
            }

            // RECURSION: Finally, dive down into the rightmost child
            print_projected_rows(rightmost_child_page, col_indices, filter_col_idx, filter_val);
        }
        else {
            cerr << "Unsupported B-Tree page type: 0x" << hex << (int)page_type << " at page " << page_number << endl;
            exit(1);
        }
    }
    
    // 1. Search sqlite_schema to see if an index exists for a specific table & column
    int get_index_root_page(const string& table_name, const string& col_name) {
        for (int i = 0; i < cell_count; i++) {
            stream.seekg(108 + (i * 2));
            unsigned short cell_offset = big_endian(stream, 2);
            stream.seekg(cell_offset);

            parse_varint(stream); // payload size
            parse_varint(stream); // rowid
            vector<string> row = parse_record(stream);

            // Columns: 0:type, 1:name, 2:tbl_name, 3:rootpage, 4:sql
            if (row.size() >= 5 && row[0] == "index" && row[2] == table_name) {
                string sql_lower = row[4];
                for (char &c : sql_lower) if (c >= 'A' && c <= 'Z') c += 32;
                string col_lower = col_name;
                for (char &c : col_lower) if (c >= 'A' && c <= 'Z') c += 32;

                size_t p_start = sql_lower.find('(');
                if (p_start != string::npos && sql_lower.find(col_lower, p_start) != string::npos) {
                    return stoi(row[3]);
                }
            }
        }
        return -1; // No index found
    }

    // 2. Fast B-Tree Index Traversal: Collects all RowIDs matching 'target_key'
    void collect_rowids_from_index(int page_number, const string& target_key, vector<int64_t>& matching_rowids) {
        size_t page_start = (page_number - 1) * this->page_size;
        stream.seekg(page_start);
        unsigned char page_type;
        stream.read((char*)&page_type, 1);

        if (page_type == 0x0A) {
            // CASE A: LEAF INDEX PAGE
            stream.seekg(page_start + 3);
            unsigned short page_cells = big_endian(stream, 2);
            size_t cell_ptr_start = page_start + 8;

            for (int i = 0; i < page_cells; i++) {
                stream.seekg(cell_ptr_start + (i * 2));
                unsigned short cell_offset = big_endian(stream, 2);
                stream.seekg(page_start + cell_offset);

                parse_varint(stream); // payload size
                vector<string> row = parse_record(stream);

                if (!row.empty()) {
                    string cell_key = row[0];
                    if (cell_key == target_key && row.size() >= 2) {
                        matching_rowids.push_back(stoll(row.back()));
                    } else if (cell_key > target_key) {
                        break; // Index is sorted; no more matches possible
                    }
                }
            }
        }
        else if (page_type == 0x02) {
            // CASE B: INTERIOR INDEX PAGE
            stream.seekg(page_start + 3);
            unsigned short page_cells = big_endian(stream, 2);
            stream.seekg(page_start + 8);
            unsigned int rightmost_child = big_endian(stream, 4);

            size_t cell_ptr_start = page_start + 12;
            bool broke = false;

            for (int i = 0; i < page_cells; i++) {
                stream.seekg(cell_ptr_start + (i * 2));
                unsigned short cell_offset = big_endian(stream, 2);
                stream.seekg(page_start + cell_offset);

                unsigned int left_child = big_endian(stream, 4);
                parse_varint(stream); // payload size
                vector<string> row = parse_record(stream);

                if (!row.empty()) {
                    string cell_key = row[0];
                    if (cell_key == target_key) {
                        if (row.size() >= 2) matching_rowids.push_back(stoll(row.back()));
                        collect_rowids_from_index(left_child, target_key, matching_rowids);
                    } else if (cell_key > target_key) {
                        collect_rowids_from_index(left_child, target_key, matching_rowids);
                        broke = true;
                        break;
                    }
                }
            }
            if (!broke) collect_rowids_from_index(rightmost_child, target_key, matching_rowids);
        }
        else {
            cerr << "Unexpected index page type: 0x" << hex << (int)page_type << endl;
            exit(1);
        }
    }

    // 3. O(log N) Binary Table Point-Lookup by RowID
    vector<string> find_row_by_rowid(int page_number, int64_t target_rowid) {
        size_t page_start = (page_number - 1) * this->page_size;
        size_t header_tax = (page_number == 1) ? 100 : 0;
        size_t btree_header = page_start + header_tax;

        stream.seekg(btree_header);
        unsigned char page_type;
        stream.read((char*)&page_type, 1);

        if (page_type == 0x0D) {
            // LEAF TABLE PAGE
            stream.seekg(btree_header + 3);
            unsigned short page_cells = big_endian(stream, 2);
            size_t cell_ptr_start = btree_header + 8;

            for (int i = 0; i < page_cells; i++) {
                stream.seekg(cell_ptr_start + (i * 2));
                unsigned short cell_offset = big_endian(stream, 2);
                stream.seekg(page_start + cell_offset);

                parse_varint(stream); // payload size
                int64_t row_id = parse_varint(stream).first;

                if (row_id == target_rowid) {
                    vector<string> row = parse_record(stream);
                    if (!row.empty()) {
                        string& col_zero = row[0];
                        if (col_zero.empty() || col_zero == "NULL" || col_zero == "null" || col_zero == "None") {
                            col_zero = to_string(row_id);
                        }
                    }
                    return row;
                } else if (row_id > target_rowid) {
                    break;
                }
            }
            return {};
        }
        else if (page_type == 0x05) {
            // INTERIOR TABLE PAGE
            stream.seekg(btree_header + 3);
            unsigned short page_cells = big_endian(stream, 2);
            stream.seekg(btree_header + 8);
            unsigned int rightmost_child = big_endian(stream, 4);

            size_t cell_ptr_start = btree_header + 12;

            for (int i = 0; i < page_cells; i++) {
                stream.seekg(cell_ptr_start + (i * 2));
                unsigned short cell_offset = big_endian(stream, 2);
                stream.seekg(page_start + cell_offset);

                unsigned int left_child = big_endian(stream, 4);
                int64_t max_rowid_in_subtree = parse_varint(stream).first;

                if (target_rowid <= max_rowid_in_subtree) {
                    return find_row_by_rowid(left_child, target_rowid);
                }
            }
            return find_row_by_rowid(rightmost_child, target_rowid);
        }
        return {};
    }

};

int main(int argc, char *argv[]) {
    // Disable output buffering
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (argc != 3) {
        std::cerr << "Expected two arguments" << std::endl;
        return 1;
    }

    std::string database_file_path = argv[1];
    std::string command = argv[2];

    DB db(database_file_path);

    if (command == ".dbinfo") {
        cout << "database page size: " << db.page_size << endl;
        cout << "number of tables: " << db.cell_count << endl;
        return 0;
    } 
    else if (command == ".tables") {
        db.print_table_names();
        return 0;
    }
    else if (command.find("SELECT COUNT") != string::npos) {
        // Quick & dirty shortcut: grab the very last word after the final space
        size_t last_space = command.rfind(' ');
        string table_name = command.substr(last_space + 1);

        // Strip off a trailing semicolon or quote just in case the tester gets funny
        while (!table_name.empty() && (table_name.back() == ';' || table_name.back() == '"' || table_name.back() == '\'')) {
            table_name.pop_back();
        }

        int root_page = db.get_table_root_page(table_name);
        unsigned short row_count = db.get_page_cell_count(root_page);

        cout << row_count << endl;
        return 0;
    }
    else if (command.find("SELECT ") == 0 && command.find(" COUNT") == string::npos) {
        string query = command.substr(7); // e.g. "name, color FROM apples WHERE color = 'Yellow'"
        
        size_t from_pos = query.find(" FROM ");
        string raw_cols = query.substr(0, from_pos); 
        string remainder = query.substr(from_pos + 6); // "apples WHERE color = 'Yellow'"

        string table_name;
        string where_col_name = "";
        string where_val = "";
        int filter_idx = -1;

        // 1. Check if a WHERE clause lives inside this query
        size_t where_pos = remainder.find(" WHERE ");
        if (where_pos != string::npos) {
            table_name = remainder.substr(0, where_pos);
            string raw_where = remainder.substr(where_pos + 7); // "color = 'Yellow'"

            size_t eq_pos = raw_where.find('=');
            if (eq_pos != string::npos) {
                where_col_name = raw_where.substr(0, eq_pos);
                where_val = raw_where.substr(eq_pos + 1);

                // Trim spaces off the column name
                size_t c_start = where_col_name.find_first_not_of(" \t");
                size_t c_end = where_col_name.find_last_not_of(" \t");
                if (c_start != string::npos) where_col_name = where_col_name.substr(c_start, c_end - c_start + 1);

                // Trim spaces AND strip the single quotes off the target value ('Yellow' -> Yellow)
                size_t v_start = where_val.find_first_not_of(" \t");
                size_t v_end = where_val.find_last_not_of(" \t");
                if (v_start != string::npos) {
                    where_val = where_val.substr(v_start, v_end - v_start + 1);
                    if (!where_val.empty() && (where_val.front() == '\'' || where_val.front() == '"')) where_val.erase(0, 1);
                    if (!where_val.empty() && (where_val.back() == '\'' || where_val.back() == '"')) where_val.pop_back();
                }
            }
        } else {
            table_name = remainder; // No WHERE clause, proceed as normal
        }

        while (!table_name.empty() && (table_name.back() == ';' || table_name.back() == '"' || table_name.back() == '\'')) {
            table_name.pop_back();
        }

        pair<int, string> meta = db.get_table_metadata(table_name);
        int root_page = meta.first;
        unordered_map<string, int> schema_map = parse_schema_to_map(meta.second);

        // 1. Resolve projected columns
        vector<int> projected_indices;
        stringstream ss_cols(raw_cols);
        string col_token;
        while (getline(ss_cols, col_token, ',')) {
            size_t first = col_token.find_first_not_of(" \t");
            size_t last = col_token.find_last_not_of(" \t");
            if (first == string::npos) continue;

            string clean_col = col_token.substr(first, last - first + 1);
            projected_indices.push_back(schema_map[clean_col]);
        }

        // 2. Check if an Index exists to serve this WHERE clause
        int index_root = -1;
        if (!where_col_name.empty()) {
            index_root = db.get_index_root_page(table_name, where_col_name);
        }

        if (index_root != -1) {
            // ==========================================================
            //                 FAST PATH: O(log N) INDEX SCAN
            // ==========================================================
            vector<int64_t> matching_rowids;
            db.collect_rowids_from_index(index_root, where_val, matching_rowids);

            for (int64_t rowid : matching_rowids) {
                vector<string> row = db.find_row_by_rowid(root_page, rowid);
                if (!row.empty()) {
                    for (size_t k = 0; k < projected_indices.size(); k++) {
                        int c_idx = projected_indices[k];
                        cout << (c_idx < row.size() ? row[c_idx] : "");
                        if (k < projected_indices.size() - 1) cout << "|";
                    }
                    cout << endl;
                }
            }
        } else {
            // ==========================================================
            //               FALLBACK: NORMAL TABLE B-TREE SCAN
            // ==========================================================
            if (!where_col_name.empty()) filter_idx = schema_map[where_col_name];
            db.print_projected_rows(root_page, projected_indices, filter_idx, where_val);
        }
        return 0;
    }
    else {
        QueryParser qp(command);
        // Put your count/query routing logic here
    }
    
    return 0;
}
