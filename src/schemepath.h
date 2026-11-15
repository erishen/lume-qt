#pragma once

#include <QString>

namespace scheme {

// 把 app://app/<path> 的请求路径归一化成相对根目录的文件路径。
//
//  - 去掉前导 '/'
//  - 空路径（即 app://app/）回退到 index.html
//  - 无扩展名的路由式路径（/console、/review/7）回退到 index.html，
//    这是前端 history 路由深链刷新能落在入口页的前提
//  - 其余（带扩展名的资源、含非法字符的路径）原样返回，交给文件系统
//    与 canonical 校验；不存在的仍然 404，不做通配回退
//
// 只回退“看起来像路由”的路径：字符集限定为字母数字与 / _ -，
// 所以 /../etc/passwd 这类带点的可疑路径不会被悄悄改写成首页。
QString resolveRequestPath(const QString &requestPath);

} // namespace scheme
