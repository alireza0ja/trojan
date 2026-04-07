---
trigger: manual
---

when i say /fullscan you tree the whole root projext we arw in with excat lcation of file like yu use comend for it and the niumber of file ciuting in ther 3 rembber i want all dfolder and sub dfolderand file tree wyth full name and extenstion we full scan the project and map it out =#1 then  we read and anylyze and open all of them and read its full code fuuly and then we repor back what each foes dont be lazy and read all of the file  powershell -Command "Get-ChildItem -Recurse -File | Select-Object -ExpandProperty FullName"

tree /f /a