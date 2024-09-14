This is an alarm clock with in-bed detection that is WiFi configured. The alarm continues to ring as long as the occupant stays in bed. It uses on a force sensing resistor (model SF15 600) that sits under my mattress.

```
curl -F "upload=@index.html" http://10.0.0.174/upload

curl -X GET http://10.0.0.174/getAlarms | jq

curl -X POST http://10.0.0.174/upsertAlarm -H "Content-Type: application/json" -d '{
  "id": 1,
  "daysOfWeek0IsSunday": [1, 2, 3, 4, 5],
  "hour": 7,
  "minute": 30,
  "isEnabled": true
}'

curl -X POST "http://10.0.0.174/deleteAlarm?id=1

curl "http://10.0.0.174/setTZ?tz=PST8PDT,M3.2.0,M11.1.0"

curl "http://10.0.0.174/getTZ"

curl "http://10.0.0.174/getLocal"

curl -X POST http://10.0.0.174/setLoudness -H "Content-Type: application/json" -d '{"amplitude": 0.2}'

curl http://10.0.0.174/testAlarm


```

