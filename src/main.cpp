#include <QApplication>

#include "model/document.hpp"
#include "ui/mainwindow.hpp"

int
main(int argc, char **argv)
{
   QApplication app(argc, argv);
   QApplication::setApplicationName("eXamineLink");
   QApplication::setOrganizationName("eXamineLink");
   xl::dictionary_folder();
   MainWindow w;
   w.show();
   const QStringList args = QApplication::arguments();
   if (args.size() > 1)
      w.openPath(args.at(1));
   return QApplication::exec();
}
