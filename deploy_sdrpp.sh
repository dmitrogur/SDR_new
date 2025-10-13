#!/bin/bash

INSTALL_DIR="./install"

# Исключаемые директории
EXCLUDE_DIRS=(
    "./source_modules/aster_server_source"
    "./source_modules/sdrpp_server_source"
    "./source_modules/tcp_client_source"
)

# Целевые директории
ASTER_TARGETS=(
    "/home/dgm/WORK/GitMalva/malva/build_Aster_ARM"
    "/home/dgm/WORK/GitMalva/malva/build_Aster_Server"
    "/home/dgm/WORK/AsterARM"
    "/home/dgm/WORK/build_Aster_Server"
)
MALVA_TARGETS=(
    "/home/dgm/WORK/GitMalva/malva/build_Malva_ARM"
    "/home/dgm/WORK/GitMalva/malva/build_Malva_Server"
)

# Отдельные цели для AsterARM и MalvaARM
ASTER_ARM_TARGET="/home/dgm/WORK/GitMalva/malva/build_Aster_ARM"
MALVA_ARM_TARGET="/home/dgm/WORK/GitMalva/malva/build_Malva_ARM"

# Очистка install
rm -rf "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR"

# Формируем параметры исключения для find
FIND_EXCLUDES=()
for dir in "${EXCLUDE_DIRS[@]}"; do
    FIND_EXCLUDES+=( -path "$dir" -prune -o )
done

# Копируем все .so* файлы
find . -type f -name "*.so*" | while read -r so_file; do
    skip=false
    for excl in "${EXCLUDE_DIRS[@]}"; do
        if [[ "$so_file" == $excl/* ]]; then
            skip=true
            break
        fi
    done
    if [ "$skip" = false ]; then
        rel_path="${so_file#./}"
        dest_path="$INSTALL_DIR/$rel_path"
        mkdir -p "$(dirname "$dest_path")"
        cp "$so_file" "$dest_path"
    fi
done

# Ищем исполняемый файл sdrpp
SDRPP_PATHS=()
while read -r exe_file; do
    SDRPP_PATHS+=("$exe_file")
done < <(eval find . "${FIND_EXCLUDES[@]}" -type f -name "sdrpp" -executable -print)

# Копируем исполняемый файл с разными именами
if [[ ${#SDRPP_PATHS[@]} -gt 0 ]]; then
    cp "${SDRPP_PATHS[0]}" "$INSTALL_DIR/Aster"
    cp "${SDRPP_PATHS[0]}" "$INSTALL_DIR/Malva"
    cp "${SDRPP_PATHS[0]}" "$INSTALL_DIR/AsterARM"
    cp "${SDRPP_PATHS[0]}" "$INSTALL_DIR/MalvaARM"
else
    echo "⚠️ Файл sdrpp не найден."
fi

# Копируем install → Aster целевые директории (исключая Malva и ARM файлы)
for target in "${ASTER_TARGETS[@]}"; do
    echo "📁 Копирование в $target..."
    rsync -a --exclude 'Malva*' --exclude 'AsterARM' "$INSTALL_DIR"/ "$target"/
done

# Копируем install → Malva целевые директории (исключая Aster и ARM файлы)
for target in "${MALVA_TARGETS[@]}"; do
    echo "📁 Копирование в $target..."
    rsync -a --exclude 'Aster*' --exclude 'MalvaARM' "$INSTALL_DIR"/ "$target"/
done

# Копируем отдельно AsterARM
if [[ -f "$INSTALL_DIR/AsterARM" ]]; then
    echo "📄 Копирование AsterARM в $ASTER_ARM_TARGET..."
    cp "$INSTALL_DIR/AsterARM" "$ASTER_ARM_TARGET/"
fi

# Копируем отдельно MalvaARM
if [[ -f "$INSTALL_DIR/MalvaARM" ]]; then
    echo "📄 Копирование MalvaARM в $MALVA_ARM_TARGET..."
    cp "$INSTALL_DIR/MalvaARM" "$MALVA_ARM_TARGET/"
fi

echo "✅ Готово: все файлы скопированы."

# Очистка install
rm -rf "$INSTALL_DIR"
