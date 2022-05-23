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
#include <libsolidity/lsp/HandlerBase.h>
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace solidity::lsp
{

class RenameSymbol: public HandlerBase
{
public:
	explicit RenameSymbol(LanguageServer& _server): HandlerBase(_server) {}

	void operator()(MessageID, Json::Value const&);
protected:
	// Nested class because otherwise `RenameSymbol` couldn't be easily used
	// with LanguageServer::m_handlers as `ASTConstVisitor` deletes required
	// c'tors
	struct Visitor: public frontend::ASTConstVisitor
	{
		explicit Visitor(RenameSymbol& _outer): m_outer(_outer) {}
		void endVisit(frontend::ImportDirective const& _node) override;
		void endVisit(frontend::Identifier const& _node) override { endVisitNode(_node); }
		void endVisit(frontend::IdentifierPath const& _node) override;
		void endVisit(frontend::UsingForDirective const& _node) override;

		private:
			RenameSymbol& m_outer;
	};

	void extractNameAndDeclaration(frontend::ASTNode const& _node, int _cursorBytePosition);
	void extractNameAndDeclaration(frontend::IdentifierPath const& _identifierPath, int _cursorBytePosition);
	void extractNameAndDeclaration(frontend::ImportDirective const& _importDirective, int _cursorBytePosition);
	void extractNameAndDeclaration(frontend::UsingForDirective const& _usingForDirective, int _cursorBytePosition);

	// Node to rename
	frontend::Declaration const* m_declarationToRename = nullptr;
	// Original name
	frontend::ASTString m_symbolName = {};
	// SourceUnits to search & replace symbol in
	std::vector<frontend::SourceUnit const*> m_sourceUnits = {};
	// Source locations that need to be replaced
	std::vector<langutil::SourceLocation> m_locations = {};
};

}
