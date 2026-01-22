# run server or client

echo $1

if [ "$1" = "server" ]; then
  if [ "$2" = "" ]; then
    ./build/server
  else
    ./build/server $2
  fi
elif [ "$1" = "client" ]; then
  if [ "$2" = "" ]; then
    ./build/client
  else
    ./build/client $2
  fi
fi

