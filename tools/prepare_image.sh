#!/usr/bin/env bash
# 在 Linux 上创建独立镜像；只对本脚本分配的 loop 设备写入。
set -euo pipefail
export LC_ALL=C.UTF-8
if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "用法：sudo bash tools/prepare_image.sh /路径/Ventoy2Disk.sh /路径/ventoy.img [容量MiB]" >&2
  exit 2
fi
ventoy=$(realpath "$1")
image=$(realpath -m "$2")
size=${3:-256}
[[ $EUID -eq 0 ]] || { echo "需要 root 权限分配 loop 设备" >&2; exit 2; }
[[ $size =~ ^[0-9]+$ && $size -ge 128 && $size -le 4096 ]] || { echo "容量必须为128～4096MiB" >&2; exit 2; }
[[ -f $ventoy ]] || { echo "Ventoy2Disk.sh 不存在" >&2; exit 2; }
[[ ! -e $image ]] || { echo "拒绝覆盖已有文件：$image" >&2; exit 2; }
for command in truncate losetup mkfs.fat partprobe udevadm; do
  command -v "$command" >/dev/null || { echo "缺少工具：$command" >&2; exit 2; }
done
loop=""
cleanup() { if [[ -n $loop ]]; then losetup -d "$loop"; fi; }
trap cleanup EXIT
truncate -s "${size}M" "$image"
loop=$(losetup --find --show --partscan "$image")
[[ $loop =~ ^/dev/loop[0-9]+$ ]] || { echo "分配的设备不是 loop 设备" >&2; exit 1; }
# 使用 Ventoy 自己的安装程序生成完整引导结构，不能只复制 EFI 文件。
(cd "$(dirname "$ventoy")" && bash ./Ventoy2Disk.sh -i -y "$loop")
partprobe "$loop"
udevadm settle
partition="${loop}p1"
[[ -b $partition ]] || { echo "未找到镜像第一分区" >&2; exit 1; }
# Ventoy 支持将数据分区改成 FAT32；引导分区保持原样。
mkfs.fat -F 32 -n VENTOY "$partition"
sync
echo "已创建：$image。第一分区为FAT32，可通过后端上传BIOS文件。"
