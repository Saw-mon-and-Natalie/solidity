/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#include <libsolutil/BooleanLP.h>
#include <libsolutil/Visitor.h>
#include <liblangutil/Common.h>
#include <libsolutil/CommonIO.h>

#include <variant>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::langutil;
using namespace solidity::smtutil;

struct SMTLib2Expression
{
	variant<string_view, vector<SMTLib2Expression>> data;

	string toString() const
	{
		return std::visit(GenericVisitor{
			[](string_view const& _sv) { return string{_sv}; },
			[](vector<SMTLib2Expression> const& _subExpr) {
				vector<string> formatted;
				for (auto const& item: _subExpr)
					formatted.emplace_back(item.toString());
				return "(" + joinHumanReadable(formatted, " ") + ")";
			}
		}, data);
	}
};

class SMTLib2Parser
{
public:
	SMTLib2Parser(string_view const& _data): m_data(_data) {}

	SMTLib2Expression parseExpression()
	{
		skipWhitespace();
		if (token() == '(')
		{
			advance();
			vector<SMTLib2Expression> subExpressions;
			while (token() != 0 && token() != ')')
			{
				subExpressions.emplace_back(parseExpression());
				skipWhitespace();
			}
			if (token() == ')')
				advance();
			return {subExpressions};
		}
		else
			return {parseToken()};
	}

	string_view remainingInput() const
	{
		return m_data.substr(m_pos);
	}

private:
	string_view parseToken()
	{
		skipWhitespace();
		size_t start = m_pos;
		bool isPipe = token() == '|';
		while (m_pos < m_data.size())
		{
			char c = token();
			if (isPipe && (m_pos > start && c == '|'))
			{
				advance();
				break;
			}
			else if (!isPipe && (langutil::isWhiteSpace(c) || c == '(' || c == ')'))
				break;
			advance();
		}
		return m_data.substr(start, m_pos - start);
	}

	void skipWhitespace()
	{
		while (isWhiteSpace(token()))
			advance();
	}

	char token()
	{
		return m_pos < m_data.size() ? m_data[m_pos] : 0;
	}
	void advance() { m_pos++;}

	size_t m_pos = 0;
	string_view const m_data;
};

namespace
{

string_view command(SMTLib2Expression const& _expr)
{
	vector<SMTLib2Expression> const& items = get<vector<SMTLib2Expression>>(_expr.data);
	solAssert(!items.empty());
	solAssert(holds_alternative<string_view>(items.front().data));
	return get<string_view>(items.front().data);
}

// TODO If we want to return rational here, we need smtutil::Expression to support rationals...
u256 parseRational(string_view _atom)
{
	if (_atom.size() >= 3 && _atom.at(_atom.size() - 1) == '0' && _atom.at(_atom.size() - 2) == '.')
		return parseRational(_atom.substr(0, _atom.size() - 2));
	else
		return u256(_atom);
}

smtutil::Expression toSMTUtilExpression(SMTLib2Expression const& _expr, map<string, SortPointer> const& _variableSorts)
{
	return std::visit(GenericVisitor{
		[&](string_view const& _atom) {
			if (isDigit(_atom.front()) || _atom.front() == '.')
				return Expression(parseRational(_atom).str(), {}, SortProvider::realSort);
			else
				return Expression(string(_atom), {}, _variableSorts.at(string(_atom)));
		},
		[&](vector<SMTLib2Expression> const& _subExpr) {
			SortPointer sort;
			vector<smtutil::Expression> arguments;
			string_view op = get<string_view>(_subExpr.front().data);
			if (op == "let")
			{
				// TODO would be good if we did not have to copy this here.
				map<string, SortPointer> subSorts = _variableSorts;
				solAssert(_subExpr.size() == 3);
				// We change the nesting here:
				// (let ((x1 t1) (x2 t2)) T) -> let(x1(t1), x2(t2), T)
				for (auto const& binding: get<vector<SMTLib2Expression>>(_subExpr.at(1).data))
				{
					auto const& bindingElements = get<vector<SMTLib2Expression>>(binding.data);
					solAssert(bindingElements.size() == 2);
					string_view varName = get<string_view>(bindingElements.at(0).data);
					Expression replacement = toSMTUtilExpression(bindingElements.at(1), _variableSorts);
					cerr << "Binding " << varName << " to " << replacement.toString() << endl;
					subSorts[string(varName)] = replacement.sort;
					arguments.emplace_back(Expression(string(varName), {move(replacement)}, replacement.sort));
				}
				arguments.emplace_back(toSMTUtilExpression(_subExpr.at(2), subSorts));
				sort = arguments.back().sort;
			}
			else
			{
				set<string> boolOperators{"and", "or", "not", "=", "<", ">", "<=", ">=", "=>"};
				for (size_t i = 1; i < _subExpr.size(); i++)
					arguments.emplace_back(toSMTUtilExpression(_subExpr[i], _variableSorts));
				sort =
					contains(boolOperators, op) ?
					SortProvider::boolSort :
					arguments.back().sort;
			}
			return Expression(string(op), move(arguments), move(sort));
		}
	}, _expr.data);
}

string removeComments(string _input)
{
	string result;
	auto it = _input.begin();
	auto end = _input.end();
	while (it != end)
	{
		if (*it == ';')
		{
			while (it != end && *it != '\n')
				++it;
			if (it != end)
				++it;
		}
		else
		{
			result.push_back(*it);
			it++;
		}

	}
	return result;
}

}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		cout << "Usage: solsmt <smtlib2 fil>" << endl;
		return -1;
	}

	string input = removeComments(readFileAsString(argv[1]));
	string_view inputToParse = input;

	map<string, SortPointer> variableSorts;
	BooleanLPSolver solver;
	while (!inputToParse.empty())
	{
		//cout << line << endl;
		SMTLib2Parser parser(inputToParse);
		SMTLib2Expression expr = parser.parseExpression();
		auto newInputToParse = parser.remainingInput();
		cerr << "got : " << string(inputToParse.begin(), newInputToParse.begin()) << endl;
		inputToParse = move(newInputToParse);
		cerr << " -> " << expr.toString() << endl;
		vector<SMTLib2Expression> const& items = get<vector<SMTLib2Expression>>(expr.data);
		string_view cmd = command(expr);
		if (cmd == "set-info")
			continue; // ignore
		else if (cmd == "declare-fun")
		{
			solAssert(items.size() == 4);
			string variableName = string{get<string_view>(items[1].data)};
			solAssert(get<vector<SMTLib2Expression>>(items[2].data).empty());
			string_view type = get<string_view>(items[3].data);
			solAssert(type == "Real" || type == "Bool");
			SortPointer sort = type == "Real" ? SortProvider::realSort : SortProvider::boolSort;
			variableSorts[variableName] = sort;
			solver.declareVariable(variableName, move(sort));
		}
		else if (cmd == "define-fun")
		{
			cerr << "Ignoring 'define-fun'" << endl;
		}
		else if (cmd == "assert")
		{
			solAssert(items.size() == 2);
			solver.addAssertion(toSMTUtilExpression(items[1], variableSorts));
		}
		else if (cmd == "set-logic")
		{
			// ignore - could check the actual logic.
		}
		else if (cmd == "check-sat")
		{
			auto&& [result, model] = solver.check({});
			if (result == CheckResult::SATISFIABLE)
				cout << "sat" << endl;
			else if (result == CheckResult::UNSATISFIABLE)
				cout << "unsat" << endl;
			else
				cout << "unknown" << endl;
		}
		else if (cmd == "exit")
			return 0;
		else
			solAssert(false, "Unknown instruction: " + string(cmd));
	}

	return 0;
}
