"""RoboMind Console 应用图标生成器（v2）

v1 的两个缺陷与本版修正
--------------------------------------------------------------------------
缺陷 1：信号条坐标算错。x0=0.135*S、步长 0.082*S，第 2/3 道竖线落在
        x≈221/304，而眼睛椭圆左边界是 x=205、描边宽 53 —— 竖线直接压
        在描边上、并伸进瞳孔内区，1024 下就已经糊成一团"g"形。
缺陷 2：单一 1024 母版 LANCZOS 降采样。53 px 描边缩到 16 px 只剩
        0.83 px，低于 1 个像素 —— 16 px 图标里整只眼睛消失，只剩背景。

修正 1：删掉信号条。图标只保留三个元素：透镜眼 + 虹膜 + 瞳孔。
        元素越少，小尺寸越立得住。
修正 2：透镜不用椭圆，用两段圆弧算出的**尖角 vesica**，尖端形状
        在高 DPI 下辨识度明显高于椭圆，且和常见"摄像头圆眼"区分开。
修正 3：**每个尺寸单独绘制**，不是一份母版降采样。
        ≥48 px 走"描边 + 虹膜环 + 瞳孔"；
        ≤32 px 走"实心眼形 + 挖空瞳孔"——这个像素量上描边必然糊，
        实心剪影反而清楚。
"""
import math
import os

from PIL import Image, ImageDraw

OUT = os.path.dirname(os.path.abspath(__file__))

BG = (26, 26, 46, 255)      # #1A1A2E  与应用深色主题一致
FG = (76, 141, 255, 255)    # #4C8DFF  应用主色
HI = (140, 190, 255, 255)   # #8CBEFF  高光

SS = 4                                  # 每档先按 4 倍绘制再降采样，边缘抗锯齿
LENS_W, LENS_HH = 0.375, 0.225          # 透镜半宽 / 半高（相对画布边长）
BAND = 0.052                            # 透镜描边厚度
IRIS_R, IRIS_T = 0.128, 0.044           # 虹膜环外半径 / 环厚
PUPIL_R = 0.056                         # 瞳孔半径


def lens(cx, cy, w, hh, n=240):
    """水平 vesica（尖角透镜）轮廓点。

    由两段圆弧拼成：上弧圆心 (cx, cy+dy)、下弧圆心 (cx, cy-dy)，同一半径 R。
    要求圆弧同时穿过左右尖角 (cx±w, cy) 和上下顶点 (cx, cy∓hh)，解得
        dy = (w² - hh²) / (2·hh),  R = dy + hh
    尖角对应角 t0 = acos(dy/R)，此时 cos t0 = dy/R 使 y 恰好回到 cy。
    """
    dy = (w * w - hh * hh) / (2.0 * hh)
    r = dy + hh
    t0 = math.acos(dy / r)

    pts = []
    for i in range(n + 1):                       # 上弧：左尖 → 顶点 → 右尖
        th = -t0 + 2.0 * t0 * i / n
        pts.append((cx + r * math.sin(th), cy + dy - r * math.cos(th)))
    for i in range(n + 1):                       # 下弧：右尖 → 底点 → 左尖
        th = t0 - 2.0 * t0 * i / n
        pts.append((cx + r * math.sin(th), cy - dy + r * math.cos(th)))
    return pts


def render(size):
    """按目标尺寸**单独**绘制（内部 4 倍超采样后降采样）。"""
    n = size * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # 圆角方形底：Windows 任务栏/开始菜单会自行裁切，这里给出统一圆角
    d.rounded_rectangle([0, 0, n - 1, n - 1], radius=0.22 * n, fill=BG)

    cx = cy = n / 2.0
    w, hh = LENS_W * n, LENS_HH * n

    if size <= 32:
        # 小尺寸：实心眼形剪影 + 挖空瞳孔（描边模式在这个像素量上必然糊）
        d.polygon(lens(cx, cy, w, hh), fill=FG)
        rp = 0.36 * hh
        d.ellipse([cx - rp, cy - rp, cx + rp, cy + rp], fill=BG)
    else:
        # 大尺寸：透镜描边 + 虹膜环 + 高光瞳孔
        t = BAND * n
        d.polygon(lens(cx, cy, w, hh), fill=FG)
        d.polygon(lens(cx, cy, w - t, hh - t), fill=BG)
        ri, rt = IRIS_R * n, IRIS_T * n
        d.ellipse([cx - ri, cy - ri, cx + ri, cy + ri],
                  outline=FG, width=int(round(rt)))
        rp = PUPIL_R * n
        d.ellipse([cx - rp, cy - rp, cx + rp, cy + rp], fill=HI)

    return img.resize((size, size), Image.LANCZOS)


# ICO 内嵌尺寸：16/24/32 供任务栏与列表视图，48+ 供开始菜单与资源管理器大图标
SIZES = (256, 128, 64, 48, 32, 24, 16)

frames = {s: render(s) for s in SIZES}

master = render(1024)
master.save(os.path.join(OUT, "icon-1024.png"))

for s, im in frames.items():
    im.save(os.path.join(OUT, "icon-%d.png" % s))

# 由各尺寸帧直接打包，避免 Pillow 内部再做一次降采样
frames[256].save(os.path.join(OUT, "app.ico"), format="ICO",
                 sizes=[(s, s) for s in SIZES],
                 append_images=[frames[s] for s in SIZES if s != 256])

print("OK: icon-1024.png / app.ico / 多尺寸 PNG")
for s in SIZES:
    p = os.path.join(OUT, "icon-%d.png" % s)
    print("  %6d  %s" % (os.path.getsize(p), os.path.basename(p)))
print("  %6d  %s" % (os.path.getsize(os.path.join(OUT, "app.ico")), "app.ico"))
