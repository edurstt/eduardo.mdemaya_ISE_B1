t <html>
t <head>
t <title>RTC - Hora y Fecha</title>
t <meta http-equiv="refresh" content="1">
t </head>
i pg_header.inc
t <h2 align=center><br>RTC: Hora y Fecha del Sistema</h2>
t <table border=1 width=60% align=center cellpadding=6>
t <tr bgcolor=#aaccff>
t  <th width=40%>Campo</th>
t  <th width=60%>Valor</th>
t </tr>
t <tr>
t  <td align=center><b>Hora</b></td>
c h 1 <td align=center>%s</td>
t </tr>
t <tr>
t  <td align=center><b>Fecha</b></td>
c h 2 <td align=center>%s</td>
t </tr>
t </table>
t <br>
i pg_footer.inc
.