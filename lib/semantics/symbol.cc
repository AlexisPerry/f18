#include "symbol.h"
#include "../parser/idioms.h"
#include "scope.h"
#include <memory>

namespace Fortran::semantics {

std::ostream &operator<<(std::ostream &os, const Symbol &sym) {
  os << sym.name();
  if (!sym.attrs().empty()) {
    os << ", " << sym.attrs();
  }
  os << ": ";
  std::visit(
      parser::visitors{
          [&](const UnknownDetails &x) { os << " Unknown"; },
          [&](const MainProgramDetails &x) { os << " MainProgram"; },
          [&](const ModuleDetails &x) { os << " Module"; },
          [&](const SubprogramDetails &x) {
            os << " Subprogram (";
            int n = 0;
            for (const auto &dummy : x.dummyNames()) {
              if (n++ > 0) os << ", ";
              os << dummy;
            }
            os << ')';
            if (x.resultName()) {
              os << " result(" << *x.resultName() << ')';
            }
          },
          [&](const EntityDetails &x) {
            os << " Entity";
            if (x.type()) {
              os << " type: " << *x.type();
            }
          },
      },
      sym.details_);
  return os;
}

}  // namespace Fortran::semantics