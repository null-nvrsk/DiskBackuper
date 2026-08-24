# DiskBackuper

Минимальный технический прототип на C++ для Visual Studio 2022. На текущем
этапе он читает обычный файл блоками и записывает его в сегментированный образ
EnCase 6 (`E01`) через `libewf-legacy`.

## Подготовка зависимости

Откройте PowerShell в корне репозитория и выполните:

```powershell
.\scripts\bootstrap-libewf.ps1
```

Скрипт загружает официальный `libyal/libewf-legacy`, фиксирует проверенный
коммит, генерирует решение Visual Studio 2022 и собирает `Release|x64` с
поддержкой записи. Если Python не находится через `PATH`, передайте каталог с
`python.exe` параметром `-PythonPath`.

После этого откройте `DiskBackuper.sln`, выберите `Debug | x64` и соберите
решение.

## Проверка Phase 0

Создать тестовый файл размером 512 МиБ:

```powershell
.\x64\Debug\DiskBackuper.Phase0.exe --create-test-file `
    .\test-data\phase0-source-512MiB.bin 512
```

Создать E01 с сегментами по 32 МиБ:

```powershell
New-Item -ItemType Directory .\test-output -Force
.\x64\Debug\DiskBackuper.Phase0.exe --create-e01 `
    .\test-data\phase0-source-512MiB.bin `
    .\test-output\phase0-image 32
```

Команда не перезаписывает существующий первый сегмент `.E01`. Для повторного
запуска укажите другое базовое имя или предварительно переместите созданные
сегменты.
