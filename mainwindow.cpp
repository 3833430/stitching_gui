#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "stitching.cpp"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_startButton_clicked()
{
    char* argv[] = {"Univ*.jpg", "--preview"};
    start(2, argv);
}