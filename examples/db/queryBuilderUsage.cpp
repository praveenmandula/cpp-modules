import cppm.db.odbc;
import cppm.db.querybuilder;

#include <iostream>
#include <string>

int main()
{
    // db connection string
    const std::string connectionString =
        "Driver={ODBC Driver 17 for SQL Server};"
        "Server=OTX-92FKLX3\\SQLEXPRESS;"
        "Database=PraveenTest;"
        "Trusted_Connection=Yes;";

    // db connect
    odbc::Connection db;
    if (!db.connect(connectionString))
    {
        std::cerr << "Connect failed: " << db.lastError() << "\n";
        return 1;
    }

    const std::string tableName = "dbo.[cppm.db.query_builder_demo]";

	// create table if not exists
    const std::string createSql =
        "IF OBJECT_ID(N'" + tableName + "', N'U') IS NULL "
        "CREATE TABLE " + tableName + " (id INT NOT NULL PRIMARY KEY, name NVARCHAR(50) NOT NULL, active BIT NOT NULL);"; 
    
    std::cout << "Create SQL:\n" << createSql << "\n\n"; 
    if (!db.execute(createSql))
    {
        std::cerr << "Create failed: " << db.lastError() << "\n";
        return 1;
    }

	// insert query using query builder
    const std::string insertSql = qb::insertInto(tableName)
        .columns({ "id", "name", "active" })
        .values({ "1", qb::quoteString("Praveen"), "1" })
        .build();

    std::cout << "Insert SQL:\n" << insertSql << "\n\n";
    if (!db.execute(insertSql))
    {
        std::cerr << "Insert failed: " << db.lastError() << "\n";
        return 1;
    }
    
	// update query using query builder
    const std::string updateSql = qb::update(tableName)
        .set({ "name = " + qb::quoteString("Praveen Mandula") })
        .where("id = 1")
        .build();

    std::cout << "Update SQL:\n" << updateSql << "\n\n";
    if (!db.execute(updateSql))
    {
        std::cerr << "Update failed: " << db.lastError() << "\n";
        return 1;
    }

	// select query using query builder
    const std::string selectSql = qb::select({ "id", "name", "active" })
        .from(tableName)
        .where("active = 1")
        .orderBy({ "id" })
        .build();

    std::cout << "Select SQL:\n" << selectSql << "\n\n";
    auto result = db.query(selectSql);
    if (!db.lastError().empty())
    {
        std::cerr << "Query failed: " << db.lastError() << "\n";
        return 1;
    }
	// print result
    std::cout << "Rows fetched: " << result.rows.size() << "\n";
    for (const auto& row : result.rows)
    {
        std::cout << row.get("id") << " | " << row.get("name") << "\n";
    }

	// delete query using query builder
    const std::string deleteSql = qb::deleteFrom(tableName)
        .where("id = 1")
        .build();

	// comment out delete sql to keep the table information in the ide
    std::cout << "\nDelete SQL:\n" << deleteSql << "\n\n";
    if (!db.execute(deleteSql))
    {
        std::cerr << "Delete failed: " << db.lastError() << "\n";
        return 1;
    }

	std::cout << "All operations completed successfully.\n";

    return 0;
}
