# multiselect

Select multiple strings at the same time, allowing to choose one when pasting.

- run the program passing a number of strings on the command line:
  ``multiselect 'a string' 'another' 'a third one'``
- middle click on a window to paste: the strings are shown, and pressing for
  example ``1`` the first is pasted: ``a string``

The strings can also be passed as lines of a text file on standard input:
``multiselect < file``

## example: pasting the commandline arguments

As an example, after running
``multiselect John Smith '2030 Blue Ave.' Simonville IL``
and middle-clicking on the second field of a web form, the state is this:

![multiselect screenshot](multiselect.png)

the user can click '2' to have ``Smith`` pasted.

## example: pasting tabular data from a file

The following file is to be posted to a web form that has a textfield for the
name, surname, address, city and state of residente.

```
John,Smith,2030 Blue Av.,Simonville,IL
Lucas,Ortega,99 Green St.,Springfield,OH
Robert,Pierre,1 Yellow Blvd.,Mongtown,NY
```

Instead of selecting and pasting each field of each person, a script can run
``multiselect`` on the data of each person, like this:

```
    cat data.txt | \\
    while read A;
    do
        echo "$A" | tr ',' '\\n' | multiselect
    done
```

The result is:

```
    selected strings:
       1: John
       2: Smith
       3: 2030 Blue Av.
       4: Simonville
       5: IL

    middle-click and press 1-5 to paste one
```

The data of the first person is ready: to paste the name, middle-click on the
textfield for the name in the web form and press '1'. For the surname,
middle-click on the textfield for the surname and press '2', and so on. When
the form is full and submitted, a further middle-click followed by key 'q'
terminates ``multiselect`` so that the script proceeds to the second line.

