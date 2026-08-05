import cppm.db.odbc;

#include <iostream>
#include <string>

int main()
{
    // Active example: System DSN + explicit database using Windows authentication.
    // System DSN details:
    //   Name: MysqlExpress
    //   Server: OTX-92FKLX3\SQLEXPRESS
    //   Database: PraveenTest
    
    //  valid combinations (uncomment one block if needed):

    //const std::string connectionString =
    //    "DSN=MysqlExpress;"
    //    "Database=PraveenTest;"
    //    "Trusted_Connection=Yes;"; 

    // const std::string connectionString =
    //     "DSN=MysqlExpress;"
    //     "Database=PraveenTest;"
    //     "UID=your_user;"
    //     "PWD=your_password;";

     const std::string connectionString =
         "Driver={ODBC Driver 17 for SQL Server};"
         "Server=OTX-92FKLX3\\SQLEXPRESS;"
         "Database=PraveenTest;"
         "Trusted_Connection=Yes;";

    odbc::Connection db;
    if (!db.connect(connectionString))
    {
        std::cerr << "Connect failed: " << db.lastError() << "\n";
        return 1;
    }

    // table name
    const std::string tableName = "dbo.[cppm.db.obdc_demo]";

    // Uses a quoted identifier so the table name can include dots.
    const std::string createSql =
        "IF OBJECT_ID(N'" + tableName + "', N'U') IS NULL "
        "CREATE TABLE " + tableName + " (id INT NOT NULL PRIMARY KEY, name NVARCHAR(50) NOT NULL);";
    if (!db.execute(createSql))
    {
        std::cerr << "Setup failed: " << db.lastError() << "\n";
        return 1;
    }

    const std::string insert1Sql =
        "IF NOT EXISTS (SELECT 1 FROM " + tableName + " WHERE id = 1) "
        "INSERT INTO " + tableName + " (id, name) VALUES (1, 'alpha');";
    if (!db.execute(insert1Sql))
    {
        std::cerr << "Insert failed: " << db.lastError() << "\n";
        return 1;
    }

    const std::string insert2Sql =
        "IF NOT EXISTS (SELECT 1 FROM " + tableName + " WHERE id = 2) "
        "INSERT INTO " + tableName + " (id, name) VALUES (2, 'beta');";
    if (!db.execute(insert2Sql))
    {
        std::cerr << "Insert failed: " << db.lastError() << "\n";
        return 1;
    }

    const std::string selectSql =
        "SELECT id, name FROM " + tableName + " ORDER BY id;";
    auto result = db.query(selectSql);
    if (!db.lastError().empty())
    {
        std::cerr << "Query failed: " << db.lastError() << "\n";
        return 1;
    }

    std::cout << "Rows fetched: " << result.rows.size() << "\n";
    for (const auto& row : result.rows)
    {
        std::cout << row.get("id") << " | " << row.get("name") << "\n";
    }

    return 0;
}
