module;

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cppm.db.querybuilder;

namespace
{
    std::string joinParts(const std::vector<std::string>& parts, std::string_view delimiter)
    {
        std::string joined;
        bool first = true;
        for (const auto& part : parts)
        {
            if (part.empty())
                continue;

            if (!first)
                joined += delimiter;

            joined += part;
            first = false;
        }

        return joined;
    }

    void appendMany(std::vector<std::string>& out, std::initializer_list<std::string_view> values)
    {
        for (std::string_view value : values)
        {
            if (!value.empty())
                out.emplace_back(value);
        }
    }

    std::string combineConditions(const std::vector<std::string>& conditions)
    {
        if (conditions.empty())
            return {};

        std::string combined;
        for (const auto& condition : conditions)
        {
            if (condition.empty())
                continue;

            if (!combined.empty())
                combined += ' ';

            combined += condition;
        }

        return combined;
    }
}

export namespace qb
{
[[nodiscard]] std::string quoteString(std::string_view value)
{
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');

    for (char ch : value)
    {
        quoted.push_back(ch);
        if (ch == '\'')
            quoted.push_back('\'');
    }

    quoted.push_back('\'');
    return quoted;
}

class SelectQueryBuilder
{
public:
    SelectQueryBuilder& select(std::string column)
    {
        if (!column.empty())
            mColumns.push_back(std::move(column));
        return *this;
    }

    SelectQueryBuilder& select(std::initializer_list<std::string_view> columns)
    {
        appendMany(mColumns, columns);
        return *this;
    }

    SelectQueryBuilder& from(std::string table)
    {
        mTable = std::move(table);
        return *this;
    }

    SelectQueryBuilder& where(std::string clause)
    {
        mConditions.clear();
        if (!clause.empty())
            mConditions.push_back(std::move(clause));
        return *this;
    }

    SelectQueryBuilder& andWhere(std::string clause)
    {
        if (!clause.empty())
            mConditions.push_back(mConditions.empty() ? std::move(clause) : "AND " + clause);
        return *this;
    }

    SelectQueryBuilder& orWhere(std::string clause)
    {
        if (!clause.empty())
            mConditions.push_back(mConditions.empty() ? std::move(clause) : "OR " + clause);
        return *this;
    }

    SelectQueryBuilder& groupBy(std::initializer_list<std::string_view> clauses)
    {
        appendMany(mGroupBy, clauses);
        return *this;
    }

    SelectQueryBuilder& having(std::string clause)
    {
        mHaving = std::move(clause);
        return *this;
    }

    SelectQueryBuilder& orderBy(std::initializer_list<std::string_view> clauses)
    {
        appendMany(mOrderBy, clauses);
        return *this;
    }

    [[nodiscard]] std::string build() const
    {
        if (mTable.empty())
            throw std::invalid_argument("SELECT query requires a table");

        std::string sql = "SELECT ";
        sql += mColumns.empty() ? "*" : joinParts(mColumns, ", ");
        sql += " FROM ";
        sql += mTable;

        const std::string whereClause = combineConditions(mConditions);
        if (!whereClause.empty())
        {
            sql += " WHERE ";
            sql += whereClause;
        }

        if (!mGroupBy.empty())
        {
            sql += " GROUP BY ";
            sql += joinParts(mGroupBy, ", ");
        }

        if (!mHaving.empty())
        {
            sql += " HAVING ";
            sql += mHaving;
        }

        if (!mOrderBy.empty())
        {
            sql += " ORDER BY ";
            sql += joinParts(mOrderBy, ", ");
        }

        return sql;
    }

private:
    std::vector<std::string> mColumns;
    std::string mTable;
    std::vector<std::string> mConditions;
    std::vector<std::string> mGroupBy;
    std::string mHaving;
    std::vector<std::string> mOrderBy;
};

class InsertQueryBuilder
{
public:
    explicit InsertQueryBuilder(std::string table = {})
        : mTable(std::move(table))
    {
    }

    InsertQueryBuilder& into(std::string table)
    {
        mTable = std::move(table);
        return *this;
    }

    InsertQueryBuilder& columns(std::initializer_list<std::string_view> columns)
    {
        appendMany(mColumns, columns);
        return *this;
    }

    InsertQueryBuilder& values(std::initializer_list<std::string_view> values)
    {
        appendMany(mValues, values);
        return *this;
    }

    [[nodiscard]] std::string build() const
    {
        if (mTable.empty())
            throw std::invalid_argument("INSERT query requires a table");
        if (mColumns.empty())
            throw std::invalid_argument("INSERT query requires columns");
        if (mValues.size() != mColumns.size())
            throw std::invalid_argument("INSERT query requires matching column and value counts");

        std::string sql = "INSERT INTO ";
        sql += mTable;
        sql += " (";
        sql += joinParts(mColumns, ", ");
        sql += ") VALUES (";
        sql += joinParts(mValues, ", ");
        sql += ')';
        return sql;
    }

private:
    std::string mTable;
    std::vector<std::string> mColumns;
    std::vector<std::string> mValues;
};

class UpdateQueryBuilder
{
public:
    explicit UpdateQueryBuilder(std::string table = {})
        : mTable(std::move(table))
    {
    }

    UpdateQueryBuilder& table(std::string tableName)
    {
        mTable = std::move(tableName);
        return *this;
    }

    UpdateQueryBuilder& set(std::string clause)
    {
        if (!clause.empty())
            mAssignments.push_back(std::move(clause));
        return *this;
    }

    UpdateQueryBuilder& set(std::initializer_list<std::string_view> clauses)
    {
        appendMany(mAssignments, clauses);
        return *this;
    }

    UpdateQueryBuilder& where(std::string clause)
    {
        mConditions.clear();
        if (!clause.empty())
            mConditions.push_back(std::move(clause));
        return *this;
    }

    UpdateQueryBuilder& andWhere(std::string clause)
    {
        if (!clause.empty())
            mConditions.push_back(mConditions.empty() ? std::move(clause) : "AND " + clause);
        return *this;
    }

    [[nodiscard]] std::string build() const
    {
        if (mTable.empty())
            throw std::invalid_argument("UPDATE query requires a table");
        if (mAssignments.empty())
            throw std::invalid_argument("UPDATE query requires assignments");

        std::string sql = "UPDATE ";
        sql += mTable;
        sql += " SET ";
        sql += joinParts(mAssignments, ", ");

        const std::string whereClause = combineConditions(mConditions);
        if (!whereClause.empty())
        {
            sql += " WHERE ";
            sql += whereClause;
        }

        return sql;
    }

private:
    std::string mTable;
    std::vector<std::string> mAssignments;
    std::vector<std::string> mConditions;
};

class DeleteQueryBuilder
{
public:
    explicit DeleteQueryBuilder(std::string table = {})
        : mTable(std::move(table))
    {
    }

    DeleteQueryBuilder& from(std::string table)
    {
        mTable = std::move(table);
        return *this;
    }

    DeleteQueryBuilder& where(std::string clause)
    {
        mConditions.clear();
        if (!clause.empty())
            mConditions.push_back(std::move(clause));
        return *this;
    }

    DeleteQueryBuilder& andWhere(std::string clause)
    {
        if (!clause.empty())
            mConditions.push_back(mConditions.empty() ? std::move(clause) : "AND " + clause);
        return *this;
    }

    [[nodiscard]] std::string build() const
    {
        if (mTable.empty())
            throw std::invalid_argument("DELETE query requires a table");

        std::string sql = "DELETE FROM ";
        sql += mTable;

        const std::string whereClause = combineConditions(mConditions);
        if (!whereClause.empty())
        {
            sql += " WHERE ";
            sql += whereClause;
        }

        return sql;
    }

private:
    std::string mTable;
    std::vector<std::string> mConditions;
};

[[nodiscard]] SelectQueryBuilder select(std::initializer_list<std::string_view> columns = {})
{
    SelectQueryBuilder builder;
    builder.select(columns);
    return builder;
}

[[nodiscard]] InsertQueryBuilder insertInto(std::string_view table)
{
    return InsertQueryBuilder(std::string(table));
}

[[nodiscard]] UpdateQueryBuilder update(std::string_view table)
{
    return UpdateQueryBuilder(std::string(table));
}

[[nodiscard]] DeleteQueryBuilder deleteFrom(std::string_view table)
{
    return DeleteQueryBuilder(std::string(table));
}
}
